#pragma once // 头文件保护

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

#ifdef AEGIS_HAS_OPCUA
#include <open62541.h> // 订阅回调需要 UA_Client / UA_DataValue 的完整类型
#else
struct UA_Client; // 未启用 OPC UA 时只保留指针声明
#endif

namespace aegis::infra {

// =============================================================================
// OpcUaClient —— OPC UA 客户端（订阅读取 + 写节点）
//
// 使用 open62541（third_party/open62541）实现：
//   - 连接 opc.tcp 端点；
//   - 创建 Subscription，并为每个节点建立 MonitoredItem：
//     设备值变化时由服务端推送通知，避免 100ms 轮询占带宽；
//   - 用 10ms 定时器驱动 UA_Client_run_iterate 处理网络消息，
//     每累计约 100ms 把最新缓存通过 valuesUpdated 发给上层（保持 10Hz 刷新节奏）；
//   - writeValue：把设定值写入指定节点（Double），用于下发 SET_VOUT / SET_IOUT。
//
// 如果工程配置时没有找到 open62541，则本类所有连接操作直接失败并给出提示，
// 其余功能不受影响。
// =============================================================================
class OpcUaClient : public QObject {
    Q_OBJECT

public:
    explicit OpcUaClient(QObject* parent = nullptr);
    ~OpcUaClient() override;

public slots:
    // 连接端点并按 nodeIds 顺序创建订阅项（默认 ns=1;s=Channel0..5）。
    void open(const QString& endpoint, const QStringList& nodeIds);
    void close();

    // 写节点值：index 为 nodeIds 的下标，value 以 Double 写入。
    // transaction 由调用方生成并原样回传，用于匹配"命令-应答"。
    void writeValue(quint16 transaction, int index, double value);

    // 读取指定节点的字符串值（如版本节点 ns=1;s=FirmwareVersion）。
    void readString(quint16 transaction, const QString& nodeId);

signals:
    void opened();
    void closed();
    void failed(const QString& reason);
    // 约 100ms 一次的快照，顺序与 open() 传入的 nodeIds 一致。
    void valuesUpdated(const QVector<double>& values);
    // 写节点结果：transaction 原样回传；status 0 成功，0xFF 失败。
    void writeCompleted(quint16 transaction, bool ok, quint8 status);
    // 读字符串节点结果；节点不存在/类型不是 String 时 ok=false。
    void readCompleted(quint16 transaction, bool ok, const QString& value);

private slots:
    // 10ms 驱动 UA 事件循环（处理 Publish 响应/回调），并按 100ms 节流发快照。
    void processEvents();

private:
#ifdef AEGIS_HAS_OPCUA
    // 订阅数据变化回调。open62541 是 C 接口，只能传静态函数 + void* 上下文。
    // subContext 存的是 OpcUaClient*，monContext 存的是节点下标。
    static void onDataChange(UA_Client* client, UA_UInt32 subId, void* subContext,
                             UA_UInt32 monId, void* monContext, UA_DataValue* value);
#endif

    // 清理订阅与客户端（失败路径和 close 共用）。
    void teardown();

    UA_Client* client_ = nullptr;
    QTimer* timer_ = nullptr;        // 10ms：事件循环 + 快照节流
    QStringList nodeIds_;            // 节点列表（顺序即通道顺序）
    QVector<double> latest_;         // 各节点最新值（仅通信线程访问）
    QVector<bool> hasValue_;         // 是否已收到过该节点的通知
    bool connected_ = false;
    bool dirty_ = false;             // 有新的通知等待发布
    int tickCount_ = 0;              // 定时器触发次数，用于 100ms 节流
};

} // namespace aegis::infra
