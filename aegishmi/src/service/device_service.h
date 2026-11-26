#pragma once // 头文件保护

#include "core/chunk_ring_buffer.h" // 分块环形缓冲
#include "core/device_config.h"     // 设备通道配置
#include "protocol/frame.h"         // 协议常量
#include "protocol/frame_codec.h"   // 帧解码器
#include "service/device_summary.h" // 1 秒摘要
#include "test/test_device.h"       // ITestDevice：测试引擎依赖的设备抽象

#include <QObject>  // Qt 对象基类（信号槽）
#include <QString>  // 日志/状态文本
#include <QThread>  // 工作线程：承载 TCP 通信
#include <QVector>  // Modbus 寄存器值

#include <atomic>             // 原子变量：停止/采集标志、命令序号
#include <condition_variable> // 等待命令应答
#include <cstdint>            // 定宽整数
#include <mutex>              // 保护应答状态与采集缓冲
#include <vector>             // 采集到的采样值

class QTimer; // 心跳定时器（定义在 .cpp，避免头文件引入 QtCore 细节）

namespace aegis::infra {
class TcpTransport;    // 前置声明，避免头文件互相包含
class ModbusTcpClient; // Modbus TCP 主站
class ModbusRtuClient; // Modbus RTU 主站（串口）
class OpcUaClient;     // OPC UA 客户端
class ScpiClient;      // SCPI 仪器客户端（TCP）
}

namespace aegis::service {

// 通信协议类型：自定义帧协议 / Modbus TCP / Modbus RTU / OPC UA / SCPI 仪器。
enum class ConnectionMode {
    CustomProtocol,
    ModbusTcp,
    ModbusRtu,
    OpcUa,
    Scpi,
};

// 运行统计：界面每秒刷新一次，用来展示链路健康度。
struct DeviceStats {
    std::uint64_t frames = 0;    // 已解析的数据帧数
    std::uint64_t crcErrors = 0; // CRC 校验错误次数
    std::uint64_t blocks = 0;    // 已写入缓冲的采样点数
    std::uint64_t dropped = 0;   // 环形缓冲满导致丢弃的采样点数
    std::uint64_t bytes = 0;     // 累计接收字节数
};

// =============================================================================
// DeviceService —— 设备业务服务（核心协调者）
//
// 职责：
//   1. 管理通信对象的生命周期与线程归属
//   2. 接收原始字节，用 FrameCodec 解析成帧
//   3. 把 DATA 帧按"数据块"写入 ChunkRingBuffer（每点不再是独立结构体）
//   4. 维护统计信息，并通过信号通知界面
//
// 线程模型：
//   - DeviceService 本身住在主线程（UI 线程）
//   - TcpTransport 被 moveToThread 到 thread_，网络 IO 在子线程事件循环中执行
//   - 子线程的信号以 Qt::QueuedConnection 投递回主线程，因此 onBytes() 在主线程执行
//   - 这样 UI 不会被网络读写阻塞，同时业务逻辑无需加锁
//
// 通道数/采样率来自 core::DeviceConfig（默认读取 configs/devices/active.json）。
// =============================================================================
class DeviceService : public QObject, public test::ITestDevice {
    Q_OBJECT

public:
    explicit DeviceService(QObject* parent = nullptr,
                           const core::DeviceConfig& config = core::DeviceConfig::loadActive());
    ~DeviceService() override;

    // 发起 TCP 连接（异步，结果通过 connectedChanged 信号通知）。
    void connectTcp(const QString& host, quint16 port);
    // 以 Modbus TCP 模式连接：按固定周期轮询输入寄存器（地址 0 起 6 个）。
    void connectModbusTcp(const QString& host, quint16 port, int unitId);
    // 以 Modbus RTU 模式连接串口。
    void connectModbusRtu(const QString& portName, int baudRate, int unitId);
    // 以 OPC UA 模式连接：订阅节点（默认 ns=1;s=Channel0..5），节点 0/1 为可写设定值。
    void connectOpcUa(const QString& endpoint);
    // 以 SCPI 仪器模式连接（SCPI-RAW over TCP，默认端口 5025）。
    // measureCommand 为周期查询命令（如 MEAS:VOLT:DC?），结果写入 CH0；
    // scale 用于把仪器单位换算成通道单位（如 V → mV 填 1000）。
    void connectScpi(const QString& host, quint16 port, const QString& measureCommand,
                     int pollMs, double scale = 1.0);
    // 链路不可用（如缺少厂商 VISA 驱动）：只记录日志并通知连接失败，
    // 不影响其它设备/链路的运行。
    void reportLinkUnavailable(const QString& reason);
    // 主动断开（自动判断当前协议）。
    void disconnectDevice();

    // 断线自动重连（默认开启）：连接意外断开后按 1/2/4/8/16/30 秒退避重试。
    void setAutoReconnect(bool enabled) { autoReconnect_ = enabled; }
    bool autoReconnect() const { return autoReconnect_; }

    // 报文诊断：开启后把收发的 HEX 前 96 字节写入日志（低频排障用）。
    void setHexLogging(bool enabled) { hexLogging_ = enabled; }
    bool hexLogging() const { return hexLogging_; }

    // 心跳周期与超时（默认 5s / 15s）。超时按"多久没有入站数据"判定链路假死。
    void setHeartbeatTiming(int intervalMs, int timeoutMs);

    // 当前是否处于连接状态。
    bool isConnected() const { return connected_.load(); }

    // 通道/采样率配置（界面用它显示通道名，测试引擎用它校验通道号）。
    const core::DeviceConfig& config() const { return config_; }

    // 暴露分块环形缓冲给界面消费。
    // 返回指针而不是引用，是为了明确"生命周期由本对象管理，调用方只借用"。
    core::ChunkRingBuffer* ring() { return &ring_; }

    // 只读统计信息。
    const DeviceStats& stats() const { return stats_; }

    // ---- ITestDevice 实现（供测试引擎调用）----
    // 下发命令并同步等待应答；返回 false 表示超时/未连接。
    // 注意：该函数会在引擎线程中阻塞，内部用条件变量等待，不占用 UI 线程。
    bool sendCommand(test::DeviceCommand command, int channel, int value,
                     int timeoutMs, quint8& status,
                     const QString& linkId = QString()) override;

    // 读取设备版本号：SCPI 默认 *IDN?；自定义协议发 ReadVersion 命令；
    // Modbus 读保持寄存器 0x0100 起 12 个（24 字节 ASCII）；OPC UA 读 ns=1;s=FirmwareVersion。
    bool queryText(const QString& cmd, int timeoutMs,
                   QString* value, QString* errorMessage,
                   const QString& linkId = QString()) override;

    // 采集指定通道在 windowMs 内的所有采样点；函数内部阻塞到窗口结束。
    // linkId 由 DeviceManager 的路由对象消费，单个 DeviceService 只负责自己那条链路。
    std::vector<std::int16_t> sampleWindow(int channel, int windowMs,
                                           const QString& linkId = QString()) override;

    // 实际采样率：自定义协议取设备配置，Modbus TCP 50Hz、RTU 20Hz、OPC UA 10Hz（订阅发布）。
    int sampleRateHz(const QString& linkId = QString()) const override;

    // 链路健康（界面面板使用）：最近一次入站距今毫秒，从未收到返回 -1。
    qint64 lastInboundAgeMs() const;
    // 当前重连尝试次数（0 表示未在重连）。
    int reconnectAttempts() const { return reconnectAttempt_; }

signals:
    // 连接状态变化；detail 是给界面显示的文本（"已连接"/"未连接"）。
    void connectedChanged(bool connected, const QString& detail);
    // 一条可展示给用户的日志。
    void logMessage(const QString& message);
    // 每满一秒的通道摘要（min/max/avg/count），供趋势存储。
    void summaryReady(const aegis::service::DeviceSummary& summary);

private slots:
    // 处理一段新收到的字节（可能包含 0 帧、1 帧或多帧）。
    void onBytes(const QByteArray& data);
    // 收到一批 Modbus 寄存器值（地址 0 起，依次对应 CH0..CH5）。
    void onModbusRegisters(const QVector<quint16>& registers);
    // 写操作完成（Modbus 写寄存器 / OPC UA 写节点），用于唤醒 sendCommand。
    void onWriteCompleted(quint16 transaction, bool ok, quint8 status);
    // Modbus 读保持寄存器完成（版本号），用于唤醒 queryText。
    void onModbusReadCompleted(quint16 transaction, bool ok, const QVector<quint16>& registers);
    // OPC UA 读字符串节点完成（版本号），用于唤醒 queryText。
    void onOpcUaReadCompleted(quint16 transaction, bool ok, const QString& text);
    // OPC UA 节点值更新。
    void onOpcUaValues(const QVector<double>& values);
    // SCPI 查询完成（transaction 为 0 表示周期轮询，不参与命令应答）。
    void onScpiQueryCompleted(quint16 transaction, bool ok, const QString& response);
    // SCPI 周期查询解析出的测量值（仪器原始单位）。
    void onScpiMeasurement(double value);

private:
    // 把一块交叠采样写入环形缓冲；超过单块上限时自动拆分。
    bool pushChunk(std::uint32_t timestampMs, float startSec,
                   const std::int16_t* interleaved, int pointCount);

    // 按通道 gain/offset 做线性校准（原地修改）。
    void applyCalibration(std::vector<std::int16_t>& samples) const;

    // 断线自动重连：安排一次重试（token 用于取消过期定时器）。
    void scheduleReconnect();
    // 用最近一次连接参数重新打开链路（不重置重试计数）。
    void reopen();
    // 关闭当前协议对应的通信对象（不改变重连/手动断开标记）。
    void closeCurrentLink();

    // ---- 链路心跳（EQ-03）----
    // 连接建立后每 5 秒检查一次：自定义协议主动发 Ping 帧；
    // 任意协议超过 15 秒没有任何入站数据即判定链路假死，走断线重连流程。
    void startHeartbeat();
    void stopHeartbeat();
    void sendHeartbeat();
    // 记录"最近一次收到数据"的时间（各入站槽函数开头调用）。
    void touchInbound();

    // ---- 1 秒摘要聚合 ----
    // 每处理一批采样调用一次；跨秒时自动输出上一秒的摘要。
    void accumulateSummary(const std::int16_t* interleaved, int pointCount);
    // 输出当前不完整的一秒（断线/析构时调用，避免丢最后一秒）。
    void flushSummary();

    core::DeviceConfig config_; // 通道数/采样率/通道名

    QThread thread_;                                    // 通信工作线程
    infra::TcpTransport* transport_ = nullptr;          // 自定义协议通信对象（归属 thread_）
    infra::ModbusTcpClient* modbus_ = nullptr;          // Modbus TCP 主站（归属 thread_）
    infra::ModbusRtuClient* rtu_ = nullptr;             // Modbus RTU 主站（归属 thread_）
    infra::OpcUaClient* opcua_ = nullptr;               // OPC UA 客户端（归属 thread_）
    infra::ScpiClient* scpi_ = nullptr;                 // SCPI 仪器客户端（归属 thread_）
    std::atomic<ConnectionMode> mode_{ConnectionMode::CustomProtocol};
    qint64 modbusStartMs_ = 0;                          // Modbus 会话起始时间（用于生成相对时间轴）
    protocol::FrameCodec codec_;                        // 帧解码器（只被 onBytes 使用，故无需加锁）
    // 分块缓冲：单块最多 512 点，256 个槽位（约 2.5 秒 @10kHz）。
    // 32 通道时约 8 MB，6 通道时约 1.5 MB。
    core::ChunkRingBuffer ring_;
    DeviceStats stats_;                                 // 运行统计
    std::atomic<bool> connected_{false};                // 当前连接状态（跨线程读取）

    // ---- 命令应答等待 ----
    std::atomic<std::uint16_t> nextSeq_{1}; // 命令帧序号
    std::mutex ackMutex_;                   // 保护下面三个字段
    std::condition_variable ackCv_;         // 收到应答时唤醒等待线程
    std::uint16_t ackSeq_ = 0;              // 当前正在等待的序号
    bool ackReceived_ = false;              // 是否已收到该序号应答
    quint8 ackStatus_ = 0xFF;               // 应答状态码
    QString ackText_;                       // 文本应答（Info 帧 / SCPI 查询结果）

    // ---- 采样窗口采集 ----
    std::atomic<bool> captureActive_{false}; // 是否正在采集
    std::atomic<int> captureChannel_{-1};    // 采哪个通道
    std::mutex captureMutex_;                // 保护采集缓冲
    std::vector<std::int16_t> captureSamples_; // 采集到的原始采样值

    // ---- 1 秒摘要累加器（仅主线程访问）----
    qint64 summarySec_ = -1;          // 当前正在累计的秒（Unix 秒）
    QVector<double> summarySum_;      // 各通道累加和
    QVector<int> summaryCount_;       // 各通道样点数
    QVector<int> summaryMin_;         // 各通道最小值
    QVector<int> summaryMax_;         // 各通道最大值

    // ---- 自动重连与报文诊断 ----
    bool autoReconnect_ = true;
    bool hexLogging_ = false;
    bool manualDisconnect_ = false;
    bool reconnectPending_ = false;   // 已有一次重连在排队，避免重复定时
    int reconnectAttempt_ = 0;
    int reconnectToken_ = 0;          // 每次安排/取消都会自增，过期定时器自动失效
    QString lastHost_;
    quint16 lastPort_ = 0;
    int lastUnitId_ = 1;
    QString lastSerialPort_;
    int lastBaudRate_ = 115200;
    QString lastEndpoint_;
    QString lastScpiQuery_ = QStringLiteral("MEAS:VOLT:DC?");
    int lastScpiPollMs_ = 100;
    double lastScpiScale_ = 1.0;

    // ---- 心跳 ----
    QTimer* heartbeatTimer_ = nullptr; // 周期检查（归属主线程）
    qint64 lastInboundMs_ = 0;         // 最近一次收到数据的时间（ms）
    int heartbeatIntervalMs_ = 5000;   // 心跳周期
    int heartbeatTimeoutMs_ = 15000;   // 无入站数据判定超时
};

} // namespace aegis::service
