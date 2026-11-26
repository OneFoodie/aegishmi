#include "service/device_service.h"

#include "infra/modbus_rtu_client.h" // Modbus RTU 主站
#include "infra/modbus_tcp_client.h" // Modbus TCP 主站
#include "infra/opcua_client.h"      // OPC UA 客户端
#include "infra/scpi_client.h"       // SCPI 仪器客户端（TCP）
#include "infra/tcp_transport.h"     // 需要完整类型才能 new 和连接信号

#include <QDateTime>
#include <QMetaObject> // QMetaObject::invokeMethod：跨线程调用槽函数
#include <QTimer>

#include <algorithm> // std::clamp / std::max
#include <chrono>    // 等待超时
#include <cmath>     // std::abs

namespace aegis::service {

namespace {
// 版本寄存器约定（详见 docs/07 4.1）：
//   保持寄存器 0x0100 起 12 个 = 24 字节，大端 ASCII，NUL 之后的内容忽略。
constexpr quint16 kVersionRegisterAddress = 0x0100;
constexpr quint16 kVersionRegisterCount = 12;

// 把版本寄存器解码成文本（去掉 NUL 之后的填充）。
QString decodeVersionRegisters(const QVector<quint16>& registers)
{
    QByteArray raw;
    raw.reserve(registers.size() * 2);
    for (const quint16 value : registers) {
        raw.append(static_cast<char>((value >> 8) & 0xFF));
        raw.append(static_cast<char>(value & 0xFF));
    }

    const int nul = raw.indexOf('\0');
    if (nul >= 0)
        raw.truncate(nul);
    return QString::fromUtf8(raw).trimmed();
}
} // namespace

// =============================================================================
// 构造函数：创建通信对象、迁移到子线程、建立信号槽连接、启动线程
// =============================================================================
DeviceService::DeviceService(QObject* parent, const core::DeviceConfig& config)
    : QObject(parent)
    , config_(config)
    , ring_(config.channelCount, 512, 256)
{
    // 摘要累加器按通道数初始化（仅主线程访问，无需加锁）。
    summarySum_.assign(config_.channelCount, 0.0);
    summaryCount_.assign(config_.channelCount, 0);
    summaryMin_.assign(config_.channelCount, 0);
    summaryMax_.assign(config_.channelCount, 0);
    // 1) 创建通信对象。注意：此时它仍属于主线程。
    transport_ = new infra::TcpTransport();

    // 2) 把对象"移动"到子线程。
    //    之后它的槽函数会在子线程执行，内部 socket 的 readyRead 也在子线程触发。
    transport_->moveToThread(&thread_);

    // 3) 线程结束时删除通信对象，避免泄漏。
    //    QThread::finished 在子线程事件循环退出时发出。
    connect(&thread_, &QThread::finished, transport_, &QObject::deleteLater);

    // 4) 连接成功：更新状态并通知界面。
    //    Qt::QueuedConnection 表示"把调用排到接收者所在线程的事件队列里执行"，
    //    这里是主线程。跨线程时必须用队列连接，否则会在子线程直接操作 UI，导致崩溃。
    connect(transport_, &infra::TcpTransport::opened, this, [this] {
        connected_ = true;
        reconnectAttempt_ = 0;
        reconnectPending_ = false;
        ++reconnectToken_;
        emit connectedChanged(true, QStringLiteral("已连接"));
        emit logMessage(QStringLiteral("TCP 连接建立"));
    }, Qt::QueuedConnection);

    // 5) 连接断开：同样回到主线程处理。
    connect(transport_, &infra::TcpTransport::closed, this, [this] {
        connected_ = false;
        flushSummary(); // 断线前把不完整的一秒也存下来
        emit connectedChanged(false, QStringLiteral("未连接"));
        emit logMessage(QStringLiteral("TCP 连接断开"));
        scheduleReconnect();
    }, Qt::QueuedConnection);

    // 6) 网络错误：只记录日志；连接状态由 opened/closed 负责维护。
    connect(transport_, &infra::TcpTransport::failed, this, [this](const QString& reason) {
        emit logMessage(QStringLiteral("网络错误: ") + reason);
        scheduleReconnect();
    }, Qt::QueuedConnection);

    // 7) 收到字节：交给 onBytes 解析。
    //    onBytes 是成员函数槽，队列连接会把 QByteArray 拷贝一份投递到主线程，
    //    因此 transport 的缓冲区可以立刻复用，不会有悬垂引用。
    connect(transport_, &infra::TcpTransport::bytesReceived,
            this, &DeviceService::onBytes, Qt::QueuedConnection);

    // 8) 启动线程事件循环。此后 moveToThread 过去的对象才开始"活"在子线程。
    thread_.start();

    // 9) Modbus TCP 主站：与自定义协议并存，按界面选择的模式使用。
    modbus_ = new infra::ModbusTcpClient();
    modbus_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, modbus_, &QObject::deleteLater);

    connect(modbus_, &infra::ModbusTcpClient::opened, this, [this] {
        connected_ = true;
        modbusStartMs_ = 0; // 新会话，重新计时
        reconnectAttempt_ = 0;
        reconnectPending_ = false;
        ++reconnectToken_;
        emit connectedChanged(true, QStringLiteral("已连接（Modbus TCP）"));
        emit logMessage(QStringLiteral("Modbus TCP 连接建立"));
    }, Qt::QueuedConnection);

    connect(modbus_, &infra::ModbusTcpClient::closed, this, [this] {
        connected_ = false;
        flushSummary();
        emit connectedChanged(false, QStringLiteral("未连接"));
        emit logMessage(QStringLiteral("Modbus TCP 连接断开"));
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(modbus_, &infra::ModbusTcpClient::failed, this, [this](const QString& reason) {
        emit logMessage(QStringLiteral("Modbus 错误：") + reason);
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(modbus_, &infra::ModbusTcpClient::registersUpdated,
            this, &DeviceService::onModbusRegisters, Qt::QueuedConnection);
    connect(modbus_, &infra::ModbusTcpClient::writeCompleted,
            this, &DeviceService::onWriteCompleted, Qt::QueuedConnection);
    connect(modbus_, &infra::ModbusTcpClient::readCompleted,
            this, &DeviceService::onModbusReadCompleted, Qt::QueuedConnection);

    // 10) Modbus RTU 主站（串口），结构与 TCP 版一致。
    rtu_ = new infra::ModbusRtuClient();
    rtu_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, rtu_, &QObject::deleteLater);

    connect(rtu_, &infra::ModbusRtuClient::opened, this, [this] {
        connected_ = true;
        modbusStartMs_ = 0;
        reconnectAttempt_ = 0;
        reconnectPending_ = false;
        ++reconnectToken_;
        emit connectedChanged(true, QStringLiteral("已连接（Modbus RTU）"));
        emit logMessage(QStringLiteral("Modbus RTU 串口已打开"));
    }, Qt::QueuedConnection);

    connect(rtu_, &infra::ModbusRtuClient::closed, this, [this] {
        connected_ = false;
        flushSummary();
        emit connectedChanged(false, QStringLiteral("未连接"));
        emit logMessage(QStringLiteral("Modbus RTU 连接关闭"));
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(rtu_, &infra::ModbusRtuClient::failed, this, [this](const QString& reason) {
        emit logMessage(QStringLiteral("Modbus RTU 错误：") + reason);
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(rtu_, &infra::ModbusRtuClient::registersUpdated,
            this, &DeviceService::onModbusRegisters, Qt::QueuedConnection);
    connect(rtu_, &infra::ModbusRtuClient::writeCompleted,
            this, &DeviceService::onWriteCompleted, Qt::QueuedConnection);
    connect(rtu_, &infra::ModbusRtuClient::readCompleted,
            this, &DeviceService::onModbusReadCompleted, Qt::QueuedConnection);

    // 11) OPC UA 客户端。
    opcua_ = new infra::OpcUaClient();
    opcua_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, opcua_, &QObject::deleteLater);

    connect(opcua_, &infra::OpcUaClient::opened, this, [this] {
        connected_ = true;
        modbusStartMs_ = 0;
        reconnectAttempt_ = 0;
        reconnectPending_ = false;
        ++reconnectToken_;
        emit connectedChanged(true, QStringLiteral("已连接（OPC UA）"));
        emit logMessage(QStringLiteral("OPC UA 会话建立"));
    }, Qt::QueuedConnection);

    connect(opcua_, &infra::OpcUaClient::closed, this, [this] {
        connected_ = false;
        flushSummary();
        emit connectedChanged(false, QStringLiteral("未连接"));
        emit logMessage(QStringLiteral("OPC UA 会话关闭"));
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(opcua_, &infra::OpcUaClient::failed, this, [this](const QString& reason) {
        emit logMessage(QStringLiteral("OPC UA 错误：") + reason);
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(opcua_, &infra::OpcUaClient::valuesUpdated,
            this, &DeviceService::onOpcUaValues, Qt::QueuedConnection);
    connect(opcua_, &infra::OpcUaClient::writeCompleted,
            this, &DeviceService::onWriteCompleted, Qt::QueuedConnection);
    connect(opcua_, &infra::OpcUaClient::readCompleted,
            this, &DeviceService::onOpcUaReadCompleted, Qt::QueuedConnection);

    // 12) SCPI 仪器客户端（SCPI-RAW over TCP，默认端口 5025）。
    //     周期查询结果作为 CH0 的采样点，复用"寄存器值 → 采样块"的入库链路。
    scpi_ = new infra::ScpiClient();
    scpi_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, scpi_, &QObject::deleteLater);

    connect(scpi_, &infra::ScpiClient::opened, this, [this] {
        connected_ = true;
        modbusStartMs_ = 0;
        reconnectAttempt_ = 0;
        reconnectPending_ = false;
        ++reconnectToken_;
        emit connectedChanged(true, QStringLiteral("已连接（SCPI）"));
        emit logMessage(QStringLiteral("SCPI 仪器连接建立"));
    }, Qt::QueuedConnection);

    connect(scpi_, &infra::ScpiClient::closed, this, [this] {
        connected_ = false;
        flushSummary();
        emit connectedChanged(false, QStringLiteral("未连接"));
        emit logMessage(QStringLiteral("SCPI 仪器连接断开"));
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(scpi_, &infra::ScpiClient::failed, this, [this](const QString& reason) {
        emit logMessage(QStringLiteral("SCPI 错误：") + reason);
        scheduleReconnect();
    }, Qt::QueuedConnection);

    connect(scpi_, &infra::ScpiClient::queryCompleted,
            this, &DeviceService::onScpiQueryCompleted, Qt::QueuedConnection);
    connect(scpi_, &infra::ScpiClient::measurement,
            this, &DeviceService::onScpiMeasurement, Qt::QueuedConnection);

    // 13) 链路心跳：连接建立后按周期检查入站活性（默认 5 秒一次）。
    heartbeatTimer_ = new QTimer(this);
    heartbeatTimer_->setInterval(heartbeatIntervalMs_);
    connect(heartbeatTimer_, &QTimer::timeout, this, &DeviceService::sendHeartbeat);
    connect(this, &DeviceService::connectedChanged, this,
            [this](bool connected, const QString&) {
                if (connected)
                    startHeartbeat();
                else
                    stopHeartbeat();
            });
}

// 析构：请求线程退出并等待其结束。
// 顺序很重要：必须先停线程，否则子线程可能还在回调已销毁的对象。
DeviceService::~DeviceService()
{
    flushSummary();      // 退出前保存最后不完整的一秒
    thread_.quit();      // 请求事件循环退出
    thread_.wait(3000);  // 最多等待 3 秒，防止程序卡死退出
}

// =============================================================================
// connectTcp：把"打开连接"这个动作投递到子线程执行
//
// 为什么不能直接 transport_->open(...)？
//   因为 transport_ 属于子线程，QTcpSocket 不是线程安全的，
//   必须在它所属的线程里调用，否则行为未定义。
// =============================================================================
void DeviceService::connectTcp(const QString& host, quint16 port)
{
    mode_ = ConnectionMode::CustomProtocol;
    lastHost_ = host;
    lastPort_ = port;
    manualDisconnect_ = false;
    reconnectAttempt_ = 0;
    reconnectPending_ = false;
    ++reconnectToken_;
    if (!thread_.isRunning()) // 线程没起来就什么都不做，避免 invoke 失败
        return;

    // 切换协议前先关掉 Modbus，避免两条链路同时占用设备。
    QMetaObject::invokeMethod(modbus_, [client = modbus_] {
        client->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(scpi_, [client = scpi_] {
        client->close();
    }, Qt::QueuedConnection);

    // 使用 lambda 形式跨线程调用：捕获 transport_ 与参数，在子线程执行。
    QMetaObject::invokeMethod(transport_, [transport = transport_, host, port] {
        transport->open(host, port);
    }, Qt::QueuedConnection);
}

void DeviceService::connectModbusTcp(const QString& host, quint16 port, int unitId)
{
    mode_ = ConnectionMode::ModbusTcp;
    modbusStartMs_ = 0;
    lastHost_ = host;
    lastPort_ = port;
    lastUnitId_ = unitId;
    manualDisconnect_ = false;
    reconnectAttempt_ = 0;
    reconnectPending_ = false;
    ++reconnectToken_;
    if (!thread_.isRunning())
        return;

    // 先断开其他链路（自定义协议 / RTU）。
    QMetaObject::invokeMethod(transport_, [transport = transport_] {
        transport->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(rtu_, [client = rtu_] {
        client->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(scpi_, [client = scpi_] {
        client->close();
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(modbus_, [client = modbus_, host, port, unitId] {
        client->open(host, port, unitId);
    }, Qt::QueuedConnection);
}

void DeviceService::connectModbusRtu(const QString& portName, int baudRate, int unitId)
{
    mode_ = ConnectionMode::ModbusRtu;
    modbusStartMs_ = 0;
    lastSerialPort_ = portName;
    lastBaudRate_ = baudRate;
    lastUnitId_ = unitId;
    manualDisconnect_ = false;
    reconnectAttempt_ = 0;
    reconnectPending_ = false;
    ++reconnectToken_;
    if (!thread_.isRunning())
        return;

    QMetaObject::invokeMethod(transport_, [transport = transport_] {
        transport->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(modbus_, [client = modbus_] {
        client->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(scpi_, [client = scpi_] {
        client->close();
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(rtu_, [client = rtu_, portName, baudRate, unitId] {
        client->open(portName, baudRate, unitId);
    }, Qt::QueuedConnection);
}

// 断开连接：按当前协议投递到对应对象的线程。
void DeviceService::connectOpcUa(const QString& endpoint)
{
    mode_ = ConnectionMode::OpcUa;
    modbusStartMs_ = 0;
    lastEndpoint_ = endpoint;
    manualDisconnect_ = false;
    reconnectAttempt_ = 0;
    reconnectPending_ = false;
    ++reconnectToken_;
    if (!thread_.isRunning())
        return;

    QMetaObject::invokeMethod(transport_, [transport = transport_] {
        transport->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(modbus_, [client = modbus_] {
        client->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(rtu_, [client = rtu_] {
        client->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(scpi_, [client = scpi_] {
        client->close();
    }, Qt::QueuedConnection);

    // 按设备配置生成节点列表：模拟器在同一端点上暴露 Channel0..ChannelN-1。
    QStringList nodes;
    nodes.reserve(config_.channelCount);
    for (int i = 0; i < config_.channelCount; ++i)
        nodes.append(QStringLiteral("ns=1;s=Channel%1").arg(i));

    QMetaObject::invokeMethod(opcua_, [client = opcua_, endpoint, nodes] {
        client->open(endpoint, nodes);
    }, Qt::QueuedConnection);
}

// =============================================================================
// connectScpi：以 SCPI 仪器模式连接（SCPI-RAW over TCP）
//
// 周期查询命令（默认 MEAS:VOLT:DC?）由 ScpiClient 自己按 pollMs 轮询，
// 解析出的数值乘以 scale 后写入 CH0，复用 Modbus/OPC UA 的采样入库链路。
// =============================================================================
void DeviceService::connectScpi(const QString& host, quint16 port,
                                const QString& measureCommand, int pollMs, double scale)
{
    mode_ = ConnectionMode::Scpi;
    modbusStartMs_ = 0;
    lastHost_ = host;
    lastPort_ = port;
    lastScpiQuery_ = measureCommand.trimmed().isEmpty()
                         ? QStringLiteral("MEAS:VOLT:DC?")
                         : measureCommand.trimmed();
    lastScpiPollMs_ = std::clamp(pollMs, 50, 5000);
    lastScpiScale_ = (std::isfinite(scale) && scale != 0.0) ? scale : 1.0;
    manualDisconnect_ = false;
    reconnectAttempt_ = 0;
    reconnectPending_ = false;
    ++reconnectToken_;
    if (!thread_.isRunning())
        return;

    // 先断开其他链路，避免同一台仪器被两条通道占用。
    QMetaObject::invokeMethod(transport_, [transport = transport_] {
        transport->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(modbus_, [client = modbus_] {
        client->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(rtu_, [client = rtu_] {
        client->close();
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(opcua_, [client = opcua_] {
        client->close();
    }, Qt::QueuedConnection);

    const QString query = lastScpiQuery_;
    const int interval = lastScpiPollMs_;
    QMetaObject::invokeMethod(scpi_, [client = scpi_, host, port, query, interval] {
        client->setPolling(query, interval);
        client->open(host, port);
    }, Qt::QueuedConnection);
}

void DeviceService::reportLinkUnavailable(const QString& reason)
{
    emit logMessage(reason);
    emit connectedChanged(false, reason);
}

void DeviceService::disconnectDevice()
{
    // 手动断开：禁止自动重连，并让已排队的重连定时器失效。
    manualDisconnect_ = true;
    reconnectPending_ = false;
    ++reconnectToken_;

    if (!thread_.isRunning())
        return;

    closeCurrentLink();
}

// 按当前协议关闭对应通信对象。
void DeviceService::closeCurrentLink()
{
    if (!thread_.isRunning())
        return;

    if (mode_.load() == ConnectionMode::ModbusTcp) {
        QMetaObject::invokeMethod(modbus_, [client = modbus_] {
            client->close();
        }, Qt::QueuedConnection);
        return;
    }

    if (mode_.load() == ConnectionMode::ModbusRtu) {
        QMetaObject::invokeMethod(rtu_, [client = rtu_] {
            client->close();
        }, Qt::QueuedConnection);
        return;
    }

    if (mode_.load() == ConnectionMode::OpcUa) {
        QMetaObject::invokeMethod(opcua_, [client = opcua_] {
            client->close();
        }, Qt::QueuedConnection);
        return;
    }

    if (mode_.load() == ConnectionMode::Scpi) {
        QMetaObject::invokeMethod(scpi_, [client = scpi_] {
            client->close();
        }, Qt::QueuedConnection);
        return;
    }

    QMetaObject::invokeMethod(transport_, [transport = transport_] {
        transport->close();
    }, Qt::QueuedConnection);
}

// =============================================================================
// 自动重连
//
// 1/2/4/8/16/30 秒退避；每次安排都会自增 token，任何手动连接/断开都会让
// 旧定时器失效，避免"用户已放弃、后台还在重连"。
// =============================================================================
void DeviceService::scheduleReconnect()
{
    if (!autoReconnect_ || manualDisconnect_ || reconnectPending_)
        return;

    reconnectPending_ = true;
    const int token = ++reconnectToken_;
    const int attempt = reconnectAttempt_;
    const int delaySec = std::min(30, 1 << std::min(attempt, 4));

    emit logMessage(QStringLiteral("连接已断开，%1 秒后自动重连（第 %2 次）")
                        .arg(delaySec)
                        .arg(attempt + 1));

    QTimer::singleShot(delaySec * 1000, this, [this, token] {
        reconnectPending_ = false;
        if (token != reconnectToken_ || manualDisconnect_ || !autoReconnect_
            || connected_.load()) {
            return;
        }
        ++reconnectAttempt_;
        emit logMessage(QStringLiteral("正在自动重连…"));
        reopen();
    });
}

// 用最近一次连接参数重新打开链路（不重置重试计数）。
void DeviceService::reopen()
{
    if (!thread_.isRunning())
        return;

    switch (mode_.load()) {
    case ConnectionMode::CustomProtocol: {
        QMetaObject::invokeMethod(transport_,
                                  [transport = transport_, host = lastHost_,
                                   port = lastPort_] { transport->open(host, port); },
                                  Qt::QueuedConnection);
        break;
    }
    case ConnectionMode::ModbusTcp: {
        QMetaObject::invokeMethod(modbus_,
                                  [client = modbus_, host = lastHost_,
                                   port = lastPort_, unit = lastUnitId_] {
                                      client->open(host, port, unit);
                                  },
                                  Qt::QueuedConnection);
        break;
    }
    case ConnectionMode::ModbusRtu: {
        QMetaObject::invokeMethod(rtu_,
                                  [client = rtu_, port = lastSerialPort_,
                                   baud = lastBaudRate_, unit = lastUnitId_] {
                                      client->open(port, baud, unit);
                                  },
                                  Qt::QueuedConnection);
        break;
    }
    case ConnectionMode::OpcUa: {
        QStringList nodes;
        nodes.reserve(config_.channelCount);
        for (int i = 0; i < config_.channelCount; ++i)
            nodes.append(QStringLiteral("ns=1;s=Channel%1").arg(i));
        QMetaObject::invokeMethod(opcua_,
                                  [client = opcua_, endpoint = lastEndpoint_, nodes] {
                                      client->open(endpoint, nodes);
                                  },
                                  Qt::QueuedConnection);
        break;
    }
    case ConnectionMode::Scpi: {
        QMetaObject::invokeMethod(scpi_,
                                  [client = scpi_, host = lastHost_, port = lastPort_,
                                   query = lastScpiQuery_, interval = lastScpiPollMs_] {
                                      client->setPolling(query, interval);
                                      client->open(host, port);
                                  },
                                  Qt::QueuedConnection);
        break;
    }
    }
}

// =============================================================================
// 链路心跳（EQ-03）
//
// 目的：识别"TCP 看起来还连着、但对端已经假死"的链路。
//   - 自定义协议：每 5 秒主动发一帧 Ping，设备收到后回数据/应答即视为存活
//   - Modbus / OPC UA / SCPI：本身就有周期轮询或订阅流量，只做入站超时判定
//   - 超过 15 秒没有任何入站数据 → 判定假死，关闭链路并按退避策略重连
// =============================================================================
void DeviceService::setHeartbeatTiming(int intervalMs, int timeoutMs)
{
    heartbeatIntervalMs_ = std::max(50, intervalMs);
    heartbeatTimeoutMs_ = std::max(heartbeatIntervalMs_ * 2, timeoutMs);
    if (heartbeatTimer_)
        heartbeatTimer_->setInterval(heartbeatIntervalMs_);
}

void DeviceService::touchInbound()
{
    lastInboundMs_ = QDateTime::currentMSecsSinceEpoch();
}

void DeviceService::startHeartbeat()
{
    if (!heartbeatTimer_)
        return;
    lastInboundMs_ = QDateTime::currentMSecsSinceEpoch();
    if (!heartbeatTimer_->isActive())
        heartbeatTimer_->start();
}

void DeviceService::stopHeartbeat()
{
    if (heartbeatTimer_)
        heartbeatTimer_->stop();
}

void DeviceService::sendHeartbeat()
{
    if (!connected_.load(std::memory_order_acquire))
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastInboundMs_ == 0) {
        lastInboundMs_ = now;
        return;
    }

    if (now - lastInboundMs_ > heartbeatTimeoutMs_) {
        const qint64 silentSec = (now - lastInboundMs_) / 1000;
        stopHeartbeat();
        emit logMessage(QStringLiteral("链路 %1 秒无入站数据，判定心跳超时")
                            .arg(silentSec));

        // 与手动断开区分：这里要允许自动重连，因此不走 disconnectDevice()。
        closeCurrentLink();
        connected_ = false;
        manualDisconnect_ = false;
        reconnectPending_ = false;
        ++reconnectToken_;
        emit connectedChanged(false, QStringLiteral("心跳超时"));
        scheduleReconnect();
        return;
    }

    if (mode_.load() != ConnectionMode::CustomProtocol)
        return;

    protocol::Frame ping;
    ping.seq = nextSeq_.fetch_add(1);
    ping.type = protocol::FrameType::Ping;
    const std::vector<std::uint8_t> bytes = protocol::FrameCodec::encode(ping);
    const QByteArray data(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()));
    QMetaObject::invokeMethod(transport_, [transport = transport_, data] {
        transport->send(data);
    }, Qt::QueuedConnection);
}

// =============================================================================
// onModbusRegisters：把一批寄存器值转成一个采样块
//
// 寄存器约定（与模拟器一致）：
//   0 = 输出电压 mV，1 = 输出电流 mA，2 = 温度，3~5 预留
// 时间轴从本次 Modbus 会话开始计时，避免用 epoch 秒导致坐标轴显示天文数字。
// =============================================================================
void DeviceService::onModbusRegisters(const QVector<quint16>& registers)
{
    if (registers.isEmpty())
        return;

    touchInbound();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (modbusStartMs_ == 0)
        modbusStartMs_ = now;

    const std::uint32_t timestampMs =
        static_cast<std::uint32_t>(now - modbusStartMs_);
    const float startSec = static_cast<float>(timestampMs) / 1000.0f;

    // Modbus 每次轮询只有 1 个采样时刻：构造一行交叠数据（长度 = 通道数）。
    const int channelCount = config_.channelCount;
    std::vector<std::int16_t> values(static_cast<std::size_t>(channelCount), 0);
    for (int ch = 0; ch < channelCount && ch < registers.size(); ++ch)
        values[static_cast<std::size_t>(ch)] = static_cast<std::int16_t>(registers.at(ch));

    applyCalibration(values);

    pushChunk(timestampMs, startSec, values.data(), 1);
    ++stats_.blocks;
    ++stats_.frames;
    accumulateSummary(values.data(), 1);
    // Modbus 轮询频率低（50Hz），逐点加锁的代价可以忽略。
    if (captureActive_.load(std::memory_order_acquire)) {
        const int channel = captureChannel_.load(std::memory_order_relaxed);
        if (channel >= 0 && channel < channelCount) {
            std::lock_guard<std::mutex> lock(captureMutex_);
            if (captureActive_.load(std::memory_order_relaxed))
                captureSamples_.push_back(values[static_cast<std::size_t>(channel)]);
        }
    }
}

// =============================================================================
// onBytes：收包处理主干（运行在主线程）
//
// 步骤：
//   1. 累计接收字节数
//   2. 把字节喂给解码器；对每个解析出的帧执行回调
//   3. 只处理 DATA 帧：解析负载 -> 拆成 SampleBlock -> 写入环形缓冲
//   4. 同步统计信息
// =============================================================================
void DeviceService::onBytes(const QByteArray& data)
{
    // 报文诊断：只打印前 96 字节，避免 10kHz 数据把日志刷爆。
    if (hexLogging_ && !data.isEmpty()) {
        const QByteArray preview = data.left(96);
        emit logMessage(QStringLiteral("RX %1B: %2%3")
                            .arg(data.size())
                            .arg(QString::fromLatin1(preview.toHex(' ')))
                            .arg(data.size() > preview.size() ? QStringLiteral(" …")
                                                              : QString()));
    }

    // QByteArray::size() 返回 int，这里显式转成无符号，便于统计大流量。
    stats_.bytes += static_cast<std::uint64_t>(data.size());
    touchInbound();

    // constData() 返回 const char*，协议层要求 const uint8_t*，所以做一次 reinterpret_cast。
    // 这是安全的：两者都是 1 字节宽度的字节视图，不涉及类型双关读取。
    codec_.feed(reinterpret_cast<const std::uint8_t*>(data.constData()),
                static_cast<std::size_t>(data.size()),
                [this](const protocol::Frame& frame) {
                    // ---------- 信息帧：设备返回文本（版本号等），唤醒 queryText ----------
                    if (frame.type == protocol::FrameType::Info) {
                        std::lock_guard<std::mutex> lock(ackMutex_);
                        if (frame.seq == ackSeq_) {
                            ackText_ = QString::fromUtf8(
                                           reinterpret_cast<const char*>(frame.payload.data()),
                                           static_cast<int>(frame.payload.size()))
                                           .trimmed();
                            ackStatus_ = 0;
                            ackReceived_ = true;
                            ackCv_.notify_all();
                        }
                        return;
                    }

                    // ---------- 应答帧：唤醒正在等待的 sendCommand ----------
                    if (frame.type == protocol::FrameType::Ack) {
                        protocol::AckPayload ack;
                        if (!protocol::decodeAckPayload(frame.payload, ack))
                            return;

                        std::lock_guard<std::mutex> lock(ackMutex_);
                        // 只接受当前正在等待的序号，避免历史应答干扰。
                        if (frame.seq == ackSeq_) {
                            ackStatus_ = ack.status;
                            ackReceived_ = true;
                            ackCv_.notify_all(); // 唤醒引擎线程
                        }
                        return;
                    }

                    // 只关心数据帧；心跳/命令等控制帧在上面或发送侧处理。
                    if (frame.type != protocol::FrameType::Data)
                        return;

                    // 解析 DATA 负载；格式非法则静默丢弃（并可通过 stats 观察异常）。
                    protocol::DataPayload payload;
                    if (!protocol::decodeDataPayload(frame.payload, payload))
                        return;

                    // 通道数来自设备配置；采样值个数必须能被通道数整除。
                    const int channelCount = config_.channelCount;
                    if (channelCount <= 0
                        || payload.samples.size() % static_cast<std::size_t>(channelCount) != 0) {
                        emit logMessage(QStringLiteral("数据帧与设备配置不匹配（通道数 %1），已丢弃")
                                            .arg(channelCount));
                        return;
                    }

                    const int points = static_cast<int>(payload.samples.size()) / channelCount;
                    const float startSec = static_cast<float>(payload.timestampMs) / 1000.0f;

                    // 线性校准（配置了 gain/offset 时生效），后续采集/显示/判定都使用校准值。
                    applyCalibration(payload.samples);

                    // 若测试引擎正在采集某个通道，顺手把该通道的值收集到局部数组，
                    // 每帧只加一次锁，避免逐点加锁拖慢收包。
                    const bool capturing = captureActive_.load(std::memory_order_acquire);
                    const int captureChannel = captureChannel_.load(std::memory_order_relaxed);
                    const bool validCaptureChannel =
                        captureChannel >= 0 && captureChannel < channelCount;
                    std::vector<std::int16_t> captured;
                    if (capturing && validCaptureChannel)
                        captured.reserve(static_cast<std::size_t>(points));

                    // 整帧写入分块缓冲（内部会按单块上限拆分，满时累计丢块）。
                    pushChunk(payload.timestampMs, startSec, payload.samples.data(), points);
                    stats_.blocks += static_cast<std::uint64_t>(points);
                    accumulateSummary(payload.samples.data(), points);

                    if (capturing && validCaptureChannel) {
                        for (int i = 0; i < points; ++i) {
                            captured.push_back(
                                payload.samples[static_cast<std::size_t>(i) * channelCount
                                                + static_cast<std::size_t>(captureChannel)]);
                        }
                    }

                    if (!captured.empty()) {
                        std::lock_guard<std::mutex> lock(captureMutex_);
                        // 再次确认采集仍处于激活状态（可能刚被 sampleWindow 结束）。
                        if (captureActive_.load(std::memory_order_relaxed))
                            captureSamples_.insert(captureSamples_.end(),
                                                   captured.begin(), captured.end());
                    }
                });

    // 解码器的计数在 feed 之后更新，这里同步到统计结构。
    stats_.frames = codec_.framesDecoded();
    stats_.crcErrors = codec_.crcErrors();
}

// =============================================================================
// pushChunk：把一块交叠采样写入分块缓冲
//
// 单块上限由 ring_.maxPointsPerChunk() 决定（512 点）；超过时按时间偏移拆成多块，
// 保证每个 ChunkView 都能独立换算时间。缓冲满时累计 dropped，不阻塞收包。
// =============================================================================
bool DeviceService::pushChunk(std::uint32_t timestampMs, float startSec,
                              const std::int16_t* interleaved, int pointCount)
{
    if (interleaved == nullptr || pointCount <= 0)
        return true;

    const int maxPoints = ring_.maxPointsPerChunk();
    const int channelCount = config_.channelCount;
    const double rate = std::max(1, config_.sampleRateHz);

    bool allPushed = true;
    int offset = 0;
    while (offset < pointCount) {
        const int count = std::min(maxPoints, pointCount - offset);

        // 拆分后每一块的起始时间都要按采样率偏移，避免时间轴重叠。
        const auto msOffset = static_cast<std::uint32_t>(
            static_cast<double>(offset) * 1000.0 / rate);
        const float secOffset = static_cast<float>(static_cast<double>(offset) / rate);

        const std::int16_t* source =
            interleaved + static_cast<std::size_t>(offset) * channelCount;

        if (!ring_.push(timestampMs + msOffset, startSec + secOffset, source, count)) {
            stats_.dropped += static_cast<std::uint64_t>(count);
            allPushed = false;
        }
        offset += count;
    }
    return allPushed;
}

// =============================================================================
// applyCalibration：按通道 gain/offset 做线性补偿
//
// 现场传感器/治具存在固定偏差时，通过 active.json 的 gain/offset 校正；
// 校准后的值随数据块进入缓冲，界面显示、SPC、测试判定用的是同一份值。
// =============================================================================
void DeviceService::applyCalibration(std::vector<std::int16_t>& samples) const
{
    if (samples.empty() || !config_.hasCalibration())
        return;

    const int channelCount = std::max(1, config_.channelCount);
    if (static_cast<int>(config_.channels.size()) < channelCount)
        return;

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const core::DeviceChannel& channel = config_.channels.at(
            static_cast<int>(i % static_cast<std::size_t>(channelCount)));
        if (std::abs(channel.gain - 1.0) < 1e-9 && std::abs(channel.offset) < 1e-9)
            continue;
        const double value = samples[i] * channel.gain + channel.offset;
        samples[i] = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0));
    }
}

// =============================================================================
// accumulateSummary：把一批采样累加到"当前秒"的 min/max/avg/count
//
// 时间粒度用系统墙钟（Unix 秒），方便历史趋势按时间查询；
// 批内跨秒的误差最多一个批次（10ms），对 1 秒粒度的趋势没有影响。
// =============================================================================
void DeviceService::accumulateSummary(const std::int16_t* interleaved, int pointCount)
{
    const int channels = config_.channelCount;
    if (interleaved == nullptr || pointCount <= 0 || channels <= 0)
        return;

    const qint64 sec = QDateTime::currentSecsSinceEpoch();
    if (sec != summarySec_) {
        flushSummary(); // 输出上一秒（如果已有数据）
        summarySec_ = sec;
    }

    for (int i = 0; i < pointCount; ++i) {
        const std::int16_t* point = interleaved + static_cast<std::size_t>(i) * channels;
        for (int ch = 0; ch < channels; ++ch) {
            const int value = point[ch];
            if (summaryCount_[ch] == 0) {
                summaryMin_[ch] = value;
                summaryMax_[ch] = value;
            } else {
                summaryMin_[ch] = std::min(summaryMin_[ch], value);
                summaryMax_[ch] = std::max(summaryMax_[ch], value);
            }
            summarySum_[ch] += value;
            ++summaryCount_[ch];
        }
    }
}

// =============================================================================
// flushSummary：输出当前累计的一秒并清零
// =============================================================================
void DeviceService::flushSummary()
{
    if (summarySec_ < 0)
        return;

    DeviceSummary summary;
    summary.epochSec = summarySec_;
    summary.channels.resize(config_.channelCount);

    bool hasData = false;
    for (int ch = 0; ch < config_.channelCount; ++ch) {
        const int count = summaryCount_[ch];
        if (count <= 0)
            continue;

        ChannelSummary& channel = summary.channels[ch];
        channel.minValue = summaryMin_[ch];
        channel.maxValue = summaryMax_[ch];
        channel.avgValue = summarySum_[ch] / count;
        channel.sampleCount = count;

        summarySum_[ch] = 0.0;
        summaryCount_[ch] = 0;
        summaryMin_[ch] = 0;
        summaryMax_[ch] = 0;
        hasData = true;
    }

    summarySec_ = -1;
    if (hasData)
        emit summaryReady(summary);
}

// =============================================================================
// sendCommand：下发命令并同步等待应答（在引擎线程中阻塞）
//
// 线程交互：
//   引擎线程 -> 本函数 -> 投递到传输线程发送 -> 返回等待
//   主线程 onBytes 收到 Ack -> 条件变量唤醒本函数
// 这样 UI 线程始终不被阻塞。
// =============================================================================
bool DeviceService::sendCommand(test::DeviceCommand command, int channel, int value,
                                int timeoutMs, quint8& status, const QString& linkId)
{
    Q_UNUSED(linkId) // 每个 DeviceService 只负责一条链路，路由由 DeviceManager 完成
    if (!connected_)
        return false;

    // ---- OPC UA 模式：设定值映射为写节点 ----
    // 节点顺序与 connectOpcUa 里的约定一致：0 = 输出电压(mV)，1 = 输出电流(mA)。
    // 写操作在通信线程同步执行，结果通过 writeCompleted 回到主线程唤醒这里。
    if (mode_.load() == ConnectionMode::OpcUa) {
        int index = -1;
        switch (command) {
        case test::DeviceCommand::SetVout:
            index = 0;
            break;
        case test::DeviceCommand::SetIout:
            index = 1;
            break;
        default:
            return false; // READ_MEASURE / RESET 在 OPC UA 模式暂未实现
        }

        // 与 Modbus 写共用同一套应答等待机制；事务号同样放在 0x8000 段。
        const quint16 transaction = static_cast<quint16>(
            0x8000 | (nextSeq_.fetch_add(1) & 0x7FFF));

        {
            std::lock_guard<std::mutex> lock(ackMutex_);
            ackSeq_ = transaction;
            ackReceived_ = false;
            ackStatus_ = 0xFF;
        }

        const int nodeValue = std::clamp(value, -32768, 32767);

        QMetaObject::invokeMethod(opcua_,
                                  [client = opcua_, transaction, index, nodeValue] {
                                      client->writeValue(transaction, index,
                                                         static_cast<double>(nodeValue));
                                  },
                                  Qt::QueuedConnection);

        std::unique_lock<std::mutex> lock(ackMutex_);
        const bool received = ackCv_.wait_for(
            lock, std::chrono::milliseconds(std::max(1, timeoutMs)),
            [this] { return ackReceived_; });
        if (!received)
            return false;

        status = ackStatus_;
        ackReceived_ = false;
        return true;
    }

    // ---- SCPI 模式：测试命令映射成 SCPI 指令 ----
    // SOUR:VOLT/SOUR:CURR 用仪器单位（V/A）下发；READ_MEASURE 复用周期查询命令。
    if (mode_.load() == ConnectionMode::Scpi) {
        QByteArray commandText;
        bool expectResponse = false;
        switch (command) {
        case test::DeviceCommand::SetVout:
            commandText = QStringLiteral("SOUR:VOLT %1")
                              .arg(static_cast<double>(value) / 1000.0, 0, 'f', 3)
                              .toLatin1();
            break;
        case test::DeviceCommand::SetIout:
            commandText = QStringLiteral("SOUR:CURR %1")
                              .arg(static_cast<double>(value) / 1000.0, 0, 'f', 3)
                              .toLatin1();
            break;
        case test::DeviceCommand::ReadMeasure:
            commandText = lastScpiQuery_.toLatin1();
            expectResponse = true;
            break;
        case test::DeviceCommand::Reset:
            commandText = QByteArrayLiteral("*RST");
            break;
        default:
            return false;
        }

        const quint16 transaction = static_cast<quint16>(
            0x8000 | (nextSeq_.fetch_add(1) & 0x7FFF));
        {
            std::lock_guard<std::mutex> lock(ackMutex_);
            ackSeq_ = transaction;
            ackReceived_ = false;
            ackStatus_ = 0xFF;
        }

        if (hexLogging_)
            emit logMessage(QStringLiteral("SCPI TX: %1").arg(QString::fromLatin1(commandText)));

        if (expectResponse) {
            QMetaObject::invokeMethod(scpi_,
                                      [client = scpi_, transaction, commandText, timeoutMs] {
                                          client->query(transaction, commandText, timeoutMs);
                                      },
                                      Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(scpi_,
                                      [client = scpi_, transaction, commandText] {
                                          client->writeCommand(transaction, commandText);
                                      },
                                      Qt::QueuedConnection);
        }

        std::unique_lock<std::mutex> lock(ackMutex_);
        const bool received = ackCv_.wait_for(
            lock, std::chrono::milliseconds(std::max(1, timeoutMs)),
            [this] { return ackReceived_; });
        if (!received)
            return false;

        status = ackStatus_;
        ackReceived_ = false;
        return true;
    }

    // ---- Modbus 模式：测试命令映射成"写单个寄存器" ----
    if (mode_.load() != ConnectionMode::CustomProtocol) {
        quint16 address = 0;
        switch (command) {
        case test::DeviceCommand::SetVout:
            address = 0; // 寄存器 0 = 输出电压
            break;
        case test::DeviceCommand::SetIout:
            address = 1; // 寄存器 1 = 输出电流
            break;
        default:
            return false; // READ_MEASURE / RESET 在 Modbus 模式暂未实现
        }

        // 写事务号放在 0x8000 以上，避免与轮询事务号（1~0x7FFF）冲突。
        const std::uint16_t transaction = static_cast<std::uint16_t>(
            0x8000 | (nextSeq_.fetch_add(1) & 0x7FFF));

        {
            std::lock_guard<std::mutex> lock(ackMutex_);
            ackSeq_ = transaction;
            ackReceived_ = false;
            ackStatus_ = 0xFF;
        }

        const auto registerValue = static_cast<quint16>(std::clamp(value, 0, 65535));

        if (mode_.load() == ConnectionMode::ModbusTcp) {
            QMetaObject::invokeMethod(modbus_,
                                      [client = modbus_, transaction, address, registerValue] {
                                          client->writeRegister(transaction, address, registerValue);
                                      },
                                      Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(rtu_,
                                      [client = rtu_, transaction, address, registerValue] {
                                          client->writeRegister(transaction, address, registerValue);
                                      },
                                      Qt::QueuedConnection);
        }

        std::unique_lock<std::mutex> lock(ackMutex_);
        const bool received = ackCv_.wait_for(
            lock, std::chrono::milliseconds(std::max(1, timeoutMs)),
            [this] { return ackReceived_; });
        if (!received)
            return false;

        status = ackStatus_;
        ackReceived_ = false;
        return true;
    }

    // 把测试引擎的命令枚举映射成协议命令字。
    protocol::CommandId commandId;
    switch (command) {
    case test::DeviceCommand::SetVout:
        commandId = protocol::CommandId::SetVout;
        break;
    case test::DeviceCommand::SetIout:
        commandId = protocol::CommandId::SetIout;
        break;
    case test::DeviceCommand::ReadMeasure:
        commandId = protocol::CommandId::ReadMeasure;
        break;
    case test::DeviceCommand::Reset:
        commandId = protocol::CommandId::Reset;
        break;
    default:
        return false; // Unknown：配置写错，直接失败
    }

    protocol::CommandPayload payload;
    payload.command = commandId;
    payload.channel = static_cast<std::uint8_t>(std::clamp(channel, 0, 255));
    payload.value = static_cast<std::int16_t>(std::clamp(value, -32768, 32767));

    const std::uint16_t seq = nextSeq_.fetch_add(1);

    protocol::Frame frame;
    frame.type = protocol::FrameType::Command;
    frame.seq = seq;
    frame.payload = protocol::encodeCommandPayload(payload);

    const auto bytes = protocol::FrameCodec::encode(frame);
    const QByteArray data(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()));

    // 先登记"我在等这个序号"，再发送，避免应答回得比登记还快。
    {
        std::lock_guard<std::mutex> lock(ackMutex_);
        ackSeq_ = seq;
        ackReceived_ = false;
        ackStatus_ = 0xFF;
    }

    if (!thread_.isRunning())
        return false;

    // 投递到传输线程写 socket。
    if (hexLogging_) {
        emit logMessage(QStringLiteral("TX %1B: %2")
                            .arg(data.size())
                            .arg(QString::fromLatin1(data.left(96).toHex(' '))));
    }
    QMetaObject::invokeMethod(transport_, [transport = transport_, data] {
        transport->send(data);
    }, Qt::QueuedConnection);

    // 阻塞等待应答（带超时）。
    std::unique_lock<std::mutex> lock(ackMutex_);
    const bool received = ackCv_.wait_for(
        lock, std::chrono::milliseconds(std::max(1, timeoutMs)),
        [this] { return ackReceived_; });
    if (!received)
        return false;

    status = ackStatus_;
    ackReceived_ = false;
    return true;
}

// =============================================================================
// queryText：读取设备版本号（固件/软件版本）
//
// SCPI：发查询命令（默认 *IDN?），取回一行文本；
// 自定义协议：发 ReadVersion 命令，设备以 Info 帧回同一序号的文本；
// Modbus：读保持寄存器 0x0100 起 12 个（24 字节 ASCII）；
// OPC UA：读字符串节点 ns=1;s=FirmwareVersion。
// =============================================================================
bool DeviceService::queryText(const QString& cmd, int timeoutMs,
                              QString* value, QString* errorMessage,
                              const QString& linkId)
{
    Q_UNUSED(linkId) // 每个 DeviceService 只负责一条链路，路由由 DeviceManager 完成
    auto fail = [errorMessage](const QString& reason) {
        if (errorMessage)
            *errorMessage = reason;
        return false;
    };

    if (!connected_.load())
        return fail(QStringLiteral("设备未连接"));

    const int waitMs = std::max(1, timeoutMs);
    const auto nextTransaction = [this] {
        return static_cast<quint16>(0x8000 | (nextSeq_.fetch_add(1) & 0x7FFF));
    };

    // ---- SCPI：查询命令 → 文本响应 ----
    if (mode_.load() == ConnectionMode::Scpi) {
        const QByteArray text =
            (cmd.trimmed().isEmpty() ? QStringLiteral("*IDN?") : cmd.trimmed()).toLatin1();
        const quint16 transaction = nextTransaction();

        {
            std::lock_guard<std::mutex> lock(ackMutex_);
            ackSeq_ = transaction;
            ackReceived_ = false;
            ackStatus_ = 0xFF;
            ackText_.clear();
        }

        if (hexLogging_)
            emit logMessage(QStringLiteral("SCPI TX: %1").arg(QString::fromLatin1(text)));

        QMetaObject::invokeMethod(scpi_,
                                  [client = scpi_, transaction, text, waitMs] {
                                      client->query(transaction, text, waitMs);
                                  },
                                  Qt::QueuedConnection);

        std::unique_lock<std::mutex> lock(ackMutex_);
        const bool received = ackCv_.wait_for(
            lock, std::chrono::milliseconds(waitMs),
            [this] { return ackReceived_; });
        if (!received)
            return fail(QStringLiteral("读取版本超时"));
        if (ackStatus_ != 0)
            return fail(QStringLiteral("仪器未返回版本信息"));

        if (value)
            *value = ackText_;
        ackReceived_ = false;
        return true;
    }

    // ---- 自定义协议：ReadVersion 命令 + Info 帧文本应答 ----
    if (mode_.load() == ConnectionMode::CustomProtocol) {
        protocol::CommandPayload payload;
        payload.command = protocol::CommandId::ReadVersion;
        payload.channel = 0;
        payload.value = 0;

        const std::uint16_t seq = nextSeq_.fetch_add(1);
        protocol::Frame frame;
        frame.type = protocol::FrameType::Command;
        frame.seq = seq;
        frame.payload = protocol::encodeCommandPayload(payload);

        const auto bytes = protocol::FrameCodec::encode(frame);
        const QByteArray data(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<int>(bytes.size()));

        {
            std::lock_guard<std::mutex> lock(ackMutex_);
            ackSeq_ = seq;
            ackReceived_ = false;
            ackStatus_ = 0xFF;
            ackText_.clear();
        }

        if (!thread_.isRunning())
            return fail(QStringLiteral("通信线程未运行"));

        if (hexLogging_) {
            emit logMessage(QStringLiteral("TX %1B: %2")
                                .arg(data.size())
                                .arg(QString::fromLatin1(data.left(96).toHex(' '))));
        }
        QMetaObject::invokeMethod(transport_, [transport = transport_, data] {
            transport->send(data);
        }, Qt::QueuedConnection);

        std::unique_lock<std::mutex> lock(ackMutex_);
        const bool received = ackCv_.wait_for(
            lock, std::chrono::milliseconds(waitMs),
            [this] { return ackReceived_; });
        if (!received)
            return fail(QStringLiteral("读取版本超时"));

        if (value)
            *value = ackText_;
        ackReceived_ = false;
        return true;
    }

    // ---- Modbus（TCP/RTU）：读版本保持寄存器 ----
    if (mode_.load() == ConnectionMode::ModbusTcp
        || mode_.load() == ConnectionMode::ModbusRtu) {
        const quint16 transaction = nextTransaction();

        {
            std::lock_guard<std::mutex> lock(ackMutex_);
            ackSeq_ = transaction;
            ackReceived_ = false;
            ackStatus_ = 0xFF;
            ackText_.clear();
        }

        if (!thread_.isRunning())
            return fail(QStringLiteral("通信线程未运行"));

        if (mode_.load() == ConnectionMode::ModbusTcp) {
            QMetaObject::invokeMethod(modbus_,
                                      [client = modbus_, transaction] {
                                          client->readHoldingRegisters(
                                              transaction, kVersionRegisterAddress,
                                              kVersionRegisterCount);
                                      },
                                      Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(rtu_,
                                      [client = rtu_, transaction] {
                                          client->readHoldingRegisters(
                                              transaction, kVersionRegisterAddress,
                                              kVersionRegisterCount);
                                      },
                                      Qt::QueuedConnection);
        }

        std::unique_lock<std::mutex> lock(ackMutex_);
        const bool received = ackCv_.wait_for(
            lock, std::chrono::milliseconds(waitMs),
            [this] { return ackReceived_; });
        if (!received)
            return fail(QStringLiteral("读取版本超时"));
        if (ackStatus_ != 0 || ackText_.isEmpty())
            return fail(QStringLiteral("设备未返回版本寄存器"));

        if (value)
            *value = ackText_;
        ackReceived_ = false;
        return true;
    }

    // ---- OPC UA：读版本节点（流程里可用 cmd 覆盖默认节点） ----
    if (mode_.load() == ConnectionMode::OpcUa) {
        const QString trimmed = cmd.trimmed();
        const QString nodeId = trimmed.startsWith(QLatin1String("ns="))
            ? trimmed
            : QStringLiteral("ns=1;s=FirmwareVersion");
        const quint16 transaction = nextTransaction();

        {
            std::lock_guard<std::mutex> lock(ackMutex_);
            ackSeq_ = transaction;
            ackReceived_ = false;
            ackStatus_ = 0xFF;
            ackText_.clear();
        }

        if (!thread_.isRunning())
            return fail(QStringLiteral("通信线程未运行"));

        QMetaObject::invokeMethod(opcua_,
                                  [client = opcua_, transaction, nodeId] {
                                      client->readString(transaction, nodeId);
                                  },
                                  Qt::QueuedConnection);

        std::unique_lock<std::mutex> lock(ackMutex_);
        const bool received = ackCv_.wait_for(
            lock, std::chrono::milliseconds(waitMs),
            [this] { return ackReceived_; });
        if (!received)
            return fail(QStringLiteral("读取版本超时"));
        if (ackStatus_ != 0 || ackText_.isEmpty())
            return fail(QStringLiteral("设备未返回版本节点"));

        if (value)
            *value = ackText_;
        ackReceived_ = false;
        return true;
    }

    return fail(QStringLiteral("当前协议不支持版本读取"));
}

// =============================================================================
// sampleWindow：采集指定通道在窗口内的全部采样值
//
// 做法：置位采集标志 -> 睡眠窗口时长 -> 关闭标志并取走数据。
// 采集由 onBytes 在主线程完成，本函数只负责"开窗/关窗"。
// =============================================================================
std::vector<std::int16_t> DeviceService::sampleWindow(int channel, int windowMs,
                                                     const QString& linkId)
{
    Q_UNUSED(linkId) // 路由由 DeviceManager 完成，本对象只对应一条链路
    if (channel < 0 || channel >= config_.channelCount)
        return {};
    if (!connected_)
        return {};

    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        captureSamples_.clear();
    }
    captureChannel_.store(channel, std::memory_order_relaxed);
    captureActive_.store(true, std::memory_order_release);

    QThread::msleep(static_cast<unsigned long>(std::max(0, windowMs)));

    captureActive_.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(captureMutex_);
    return captureSamples_;
}

// =============================================================================
// onOpcUaValues：把一批节点值转成采样块（逻辑与 Modbus 一致）
// =============================================================================
void DeviceService::onOpcUaValues(const QVector<double>& values)
{
    if (values.isEmpty())
        return;

    touchInbound();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (modbusStartMs_ == 0)
        modbusStartMs_ = now;

    const std::uint32_t timestampMs =
        static_cast<std::uint32_t>(now - modbusStartMs_);
    const float startSec = static_cast<float>(timestampMs) / 1000.0f;

    const int channelCount = config_.channelCount;
    std::vector<std::int16_t> samples(static_cast<std::size_t>(channelCount), 0);
    for (int ch = 0; ch < channelCount; ++ch) {
        const double raw = ch < values.size() ? values.at(ch) : 0.0;
        samples[static_cast<std::size_t>(ch)] =
            static_cast<std::int16_t>(std::clamp(raw, -32768.0, 32767.0));
    }

    applyCalibration(samples);

    pushChunk(timestampMs, startSec, samples.data(), 1);
    ++stats_.blocks;
    ++stats_.frames;
    accumulateSummary(samples.data(), 1);

    if (captureActive_.load(std::memory_order_acquire)) {
        const int channel = captureChannel_.load(std::memory_order_relaxed);
        if (channel >= 0 && channel < channelCount) {
            std::lock_guard<std::mutex> lock(captureMutex_);
            if (captureActive_.load(std::memory_order_relaxed))
                captureSamples_.push_back(samples[static_cast<std::size_t>(channel)]);
        }
    }
}

// =============================================================================
// onScpiMeasurement：周期查询结果写入 CH0（其余通道保持 0）
//
// 复用 OPC UA 的入库逻辑；scale 把仪器单位换算成通道单位（V → mV 时填 1000）。
// =============================================================================
void DeviceService::onScpiMeasurement(double value)
{
    if (!std::isfinite(value))
        return;
    onOpcUaValues(QVector<double>{value * lastScpiScale_});
}

// =============================================================================
// onScpiQueryCompleted：唤醒等待中的 sendCommand
//
// transaction == 0 是周期轮询，不参与命令应答，直接忽略。
// =============================================================================
void DeviceService::onScpiQueryCompleted(quint16 transaction, bool ok, const QString& response)
{
    if (transaction == 0)
        return;
    if (hexLogging_ && ok)
        emit logMessage(QStringLiteral("SCPI RX: %1").arg(response));

    std::lock_guard<std::mutex> lock(ackMutex_);
    if (transaction != ackSeq_)
        return;
    ackStatus_ = ok ? 0 : 0xFF;
    ackText_ = ok ? response.trimmed() : QString();
    ackReceived_ = true;
    ackCv_.notify_all();
}

int DeviceService::sampleRateHz(const QString& linkId) const
{
    Q_UNUSED(linkId)
    switch (mode_.load()) {
    case ConnectionMode::ModbusTcp:
        return 50; // 20ms 轮询
    case ConnectionMode::ModbusRtu:
        return 20; // 50ms 轮询
    case ConnectionMode::OpcUa:
        return 10; // 订阅发布周期 100ms
    case ConnectionMode::Scpi:
        return std::max(1, 1000 / std::max(1, lastScpiPollMs_)); // 按轮询周期
    case ConnectionMode::CustomProtocol:
        break;
    }
    return config_.sampleRateHz; // 自定义协议取设备配置
}

// 链路健康：入站数据由主线程 onBytes 写入，这里也在界面线程读取，无需加锁。
qint64 DeviceService::lastInboundAgeMs() const
{
    if (lastInboundMs_ <= 0)
        return -1;
    return std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - lastInboundMs_);
}

// =============================================================================
// onWriteCompleted：写操作完成，唤醒等待中的 sendCommand
//
// 同时服务于 Modbus（写寄存器）与 OPC UA（写节点）：两者都通过事务号匹配，
// 避免把过期的写结果当成当前命令的应答。
// =============================================================================
void DeviceService::onWriteCompleted(quint16 transaction, bool ok, quint8 status)
{
    std::lock_guard<std::mutex> lock(ackMutex_);
    if (transaction != ackSeq_)
        return; // 过期应答，忽略

    ackStatus_ = ok ? status : 0xFF; // 0xFF：链路失败/设备无响应
    ackReceived_ = true;
    ackCv_.notify_all();
}

// =============================================================================
// onModbusReadCompleted：Modbus 版本寄存器读取完成，唤醒等待中的 queryText
// =============================================================================
void DeviceService::onModbusReadCompleted(quint16 transaction, bool ok,
                                          const QVector<quint16>& registers)
{
    std::lock_guard<std::mutex> lock(ackMutex_);
    if (transaction != ackSeq_ || ackReceived_)
        return; // 过期应答，忽略

    if (ok)
        ackText_ = decodeVersionRegisters(registers);
    ackStatus_ = ok ? 0 : 0xFF;
    ackReceived_ = true;
    ackCv_.notify_all();
}

// =============================================================================
// onOpcUaReadCompleted：OPC UA 版本节点读取完成，唤醒等待中的 queryText
// =============================================================================
void DeviceService::onOpcUaReadCompleted(quint16 transaction, bool ok, const QString& text)
{
    std::lock_guard<std::mutex> lock(ackMutex_);
    if (transaction != ackSeq_ || ackReceived_)
        return; // 过期应答，忽略

    if (ok)
        ackText_ = text.trimmed();
    ackStatus_ = ok ? 0 : 0xFF;
    ackReceived_ = true;
    ackCv_.notify_all();
}

} // namespace aegis::service
