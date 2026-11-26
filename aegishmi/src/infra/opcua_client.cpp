#include "infra/opcua_client.h"

#include <QTimer>

#include <cstdint> // std::intptr_t：把节点下标塞进 void* 回调上下文

#ifdef AEGIS_HAS_OPCUA
#include <open62541.h>
#endif

namespace aegis::infra {

#ifdef AEGIS_HAS_OPCUA

namespace {

constexpr int kEventLoopIntervalMs = 10;   // UA 事件循环驱动周期
constexpr int kPublishEveryTicks = 10;     // 10 × 10ms = 100ms 发一次快照
constexpr double kPublishingIntervalMs = 100.0; // 订阅发布周期
constexpr double kSamplingIntervalMs = 100.0;   // 节点采样周期

// 解析 "ns=1;s=Channel0" 形式的节点 ID。
// 兼容只写名称的情况（默认命名空间 1）。
UA_NodeId parseNodeId(const QString& text, QByteArray& storage)
{
    int nsIndex = 1;
    QString name = text;

    const QStringList parts = text.split(QLatin1Char(';'));
    for (const QString& part : parts) {
        if (part.startsWith(QLatin1String("ns=")))
            nsIndex = part.mid(3).toInt();
        else if (part.startsWith(QLatin1String("s=")))
            name = part.mid(2);
    }

    storage = name.toUtf8();
    // ALLOC 版本会复制字符串，之后需要用 UA_NodeId_clear 释放。
    return UA_NODEID_STRING_ALLOC(nsIndex, storage.constData());
}

// 从 Variant 中取出一个 double（支持常见数值类型）。
double variantToDouble(const UA_Variant& value)
{
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_DOUBLE]))
        return *static_cast<UA_Double*>(value.data);
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_FLOAT]))
        return static_cast<double>(*static_cast<UA_Float*>(value.data));
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT32]))
        return static_cast<double>(*static_cast<UA_Int32*>(value.data));
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT16]))
        return static_cast<double>(*static_cast<UA_UInt16*>(value.data));
    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT16]))
        return static_cast<double>(*static_cast<UA_Int16*>(value.data));
    return 0.0;
}

} // namespace

OpcUaClient::OpcUaClient(QObject* parent)
    : QObject(parent)
    , timer_(new QTimer(this))
{
    timer_->setInterval(kEventLoopIntervalMs);
    connect(timer_, &QTimer::timeout, this, &OpcUaClient::processEvents);
}

OpcUaClient::~OpcUaClient()
{
    close();
}

void OpcUaClient::open(const QString& endpoint, const QStringList& nodeIds)
{
    close();
    nodeIds_ = nodeIds;
    latest_ = QVector<double>(nodeIds_.size(), 0.0);
    hasValue_ = QVector<bool>(nodeIds_.size(), false);
    dirty_ = false;
    tickCount_ = 0;

    if (nodeIds_.isEmpty()) {
        emit failed(QStringLiteral("OPC UA：未配置需要订阅的节点"));
        return;
    }

    client_ = UA_Client_new();
    UA_ClientConfig* config = UA_Client_getConfig(client_);
    UA_ClientConfig_setDefault(config);

    const UA_StatusCode status =
        UA_Client_connect(client_, endpoint.toUtf8().constData());
    if (status != UA_STATUSCODE_GOOD) {
        emit failed(QStringLiteral("OPC UA 连接失败：%1")
                        .arg(QString::fromUtf8(UA_StatusCode_name(status))));
        UA_Client_delete(client_);
        client_ = nullptr;
        return;
    }

    // ---- 创建订阅：由服务端在数据变化时推送，不再逐点轮询 ----
    UA_CreateSubscriptionRequest request = UA_CreateSubscriptionRequest_default();
    request.requestedPublishingInterval = kPublishingIntervalMs;
    request.requestedMaxKeepAliveCount = 20;

    // 第二个参数传 this：回调里通过 subContext 找回客户端对象。
    UA_CreateSubscriptionResponse response =
        UA_Client_Subscriptions_create(client_, request, this, nullptr, nullptr);
    if (response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        emit failed(QStringLiteral("OPC UA 创建订阅失败：%1")
                        .arg(QString::fromUtf8(
                            UA_StatusCode_name(response.responseHeader.serviceResult))));
        teardown();
        return;
    }
    // 订阅 ID 只在建 MonitoredItem 时用到，之后由客户端内部管理。
    const UA_UInt32 subscriptionId = response.subscriptionId;

    // ---- 为每个节点建立 MonitoredItem ----
    for (int i = 0; i < nodeIds_.size(); ++i) {
        QByteArray storage;
        UA_NodeId nodeId = parseNodeId(nodeIds_.at(i), storage);

        UA_MonitoredItemCreateRequest mon =
            UA_MonitoredItemCreateRequest_default(nodeId);
        mon.requestedParameters.samplingInterval = kSamplingIntervalMs;
        mon.requestedParameters.queueSize = 4;

        // 下标塞进上下文：回调时据此知道是哪个通道。
        void* context = reinterpret_cast<void*>(static_cast<std::intptr_t>(i));

        const UA_MonitoredItemCreateResult result =
            UA_Client_MonitoredItems_createDataChange(
                client_, subscriptionId, UA_TIMESTAMPSTORETURN_BOTH, mon, context,
                &OpcUaClient::onDataChange, nullptr);

        UA_NodeId_clear(&nodeId);

        if (result.statusCode != UA_STATUSCODE_GOOD) {
            // 单个节点不可用（设备没暴露该通道）不阻断其它通道：
            // 记录错误后继续订阅剩余节点，缺失通道的快照值保持 0。
            emit failed(QStringLiteral("OPC UA 订阅节点 %1 失败：%2")
                            .arg(nodeIds_.at(i),
                                 QString::fromUtf8(UA_StatusCode_name(result.statusCode))));
            continue;
        }
    }

    connected_ = true;
    emit opened();
    timer_->start();
}

void OpcUaClient::close()
{
    teardown();
    if (connected_) {
        connected_ = false;
        emit closed();
    }
    latest_.clear();
    hasValue_.clear();
    dirty_ = false;
    tickCount_ = 0;
}

void OpcUaClient::teardown()
{
    timer_->stop();

    if (client_ == nullptr)
        return;

    // 说明：这里不显式删除订阅。会话关闭（CloseSession）时服务端会自动清理订阅；
    // 提前 DeleteSubscriptions 会让服务端把仍在途的 Publish 请求逐个回 ServiceFault，
    // 客户端随后刷出一串 "BadNoSubscription" 警告（功能无害但很吵）。
    UA_Client_disconnect(client_);
    UA_Client_delete(client_);
    client_ = nullptr;
}

void OpcUaClient::writeValue(quint16 transaction, int index, double value)
{
    if (!connected_ || client_ == nullptr || index < 0 || index >= nodeIds_.size()) {
        emit writeCompleted(transaction, false, 0xFF);
        return;
    }

    QByteArray storage;
    UA_NodeId nodeId = parseNodeId(nodeIds_.at(index), storage);

    UA_Variant variant;
    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, &value, &UA_TYPES[UA_TYPES_DOUBLE]);

    const UA_StatusCode status =
        UA_Client_writeValueAttribute(client_, nodeId, &variant);

    UA_NodeId_clear(&nodeId);

    if (status == UA_STATUSCODE_GOOD) {
        // 本地缓存同步更新：不必等下一次通知就能在界面上看到新设定值。
        latest_[index] = value;
        hasValue_[index] = true;
        dirty_ = true;
        emit writeCompleted(transaction, true, 0);
    } else {
        emit writeCompleted(transaction, false, 0xFF);
    }
}

void OpcUaClient::readString(quint16 transaction, const QString& nodeId)
{
    if (!connected_ || client_ == nullptr) {
        emit readCompleted(transaction, false, QString());
        return;
    }

    QByteArray storage;
    UA_NodeId id = parseNodeId(nodeId, storage);

    UA_Variant value;
    UA_Variant_init(&value);
    const UA_StatusCode status = UA_Client_readValueAttribute(client_, id, &value);
    UA_NodeId_clear(&id);

    QString text;
    const bool ok = status == UA_STATUSCODE_GOOD
        && UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_STRING]);
    if (ok) {
        const auto* string = static_cast<UA_String*>(value.data);
        text = QString::fromUtf8(reinterpret_cast<const char*>(string->data),
                                 static_cast<int>(string->length));
    }
    UA_Variant_clear(&value);

    emit readCompleted(transaction, ok, text);
}

void OpcUaClient::processEvents()
{
    if (!connected_ || client_ == nullptr)
        return;

    // timeout=0：只处理已经到达的数据，不阻塞事件循环。
    UA_Client_run_iterate(client_, 0);

    ++tickCount_;
    if (dirty_ && (tickCount_ % kPublishEveryTicks) == 0) {
        dirty_ = false;
        emit valuesUpdated(latest_);
    }
}

void OpcUaClient::onDataChange(UA_Client*, UA_UInt32, void* subContext,
                               UA_UInt32, void* monContext, UA_DataValue* value)
{
    auto* self = static_cast<OpcUaClient*>(subContext);
    if (self == nullptr || !self->connected_ || value == nullptr || !value->hasValue)
        return;

    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(monContext));
    if (index < 0 || index >= self->latest_.size())
        return;

    self->latest_[index] = variantToDouble(value->value);
    self->hasValue_[index] = true;
    self->dirty_ = true;
}

#else // 未启用 open62541

OpcUaClient::OpcUaClient(QObject* parent)
    : QObject(parent)
{
}

OpcUaClient::~OpcUaClient() = default;

void OpcUaClient::open(const QString&, const QStringList&)
{
    emit failed(QStringLiteral("当前构建未启用 OPC UA（缺少 third_party/open62541）"));
}

void OpcUaClient::close() = default;

void OpcUaClient::teardown() {}

void OpcUaClient::writeValue(quint16 transaction, int, double)
{
    emit writeCompleted(transaction, false, 0xFF);
}

void OpcUaClient::readString(quint16 transaction, const QString&)
{
    // 未编译 OPC UA 支持时无法读取任何节点。
    emit readCompleted(transaction, false, QString());
}

void OpcUaClient::processEvents() {}

#endif

} // namespace aegis::infra
