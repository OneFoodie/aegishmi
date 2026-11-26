#include <QCoreApplication>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm> // std::clamp
#include <cmath>
#include <random>
#include <vector>

#include "core/device_config.h"    // 设备通道配置
#include "protocol/frame.h"       // 帧结构、采样率
#include "protocol/frame_codec.h" // 帧编码/解码
#include "protocol/modbus.h"      // Modbus TCP 请求解析/响应构造

#ifdef AEGIS_HAS_OPCUA
#include <open62541.h> // OPC UA 服务端
#endif

using namespace aegis::protocol;

// =============================================================================
// DeviceSimulator —— 下位机模拟器
//
// 目的：没有真实硬件时，也能完整演示上位机的连接、采集、显示链路。
// 行为：
//   - 监听 TCP 9527 端口
//   - 每 10ms 产生 100 个采样点（即 10kHz），每点 6 个通道
//   - 按自定义协议编码成 DATA 帧发给所有已连接客户端
//   - 收到 Ping 帧时回复 Ack（演示"命令-应答"交互）
// =============================================================================

namespace {

constexpr int kTickMs = 10;          // 定时器周期：10 毫秒
constexpr int kSamplesPerTick = 100; // 每 10ms 产生 100 点 = 10kHz 采样率
constexpr double kPi = 3.14159265358979323846;

// 把 double 限制到 int16 范围内再转换，避免溢出产生未定义行为。
std::int16_t clamp16(double value)
{
    if (value > 32767.0)
        return 32767;
    if (value < -32768.0)
        return -32768;
    return static_cast<std::int16_t>(value);
}

} // namespace

class DeviceSimulator : public QObject {
    Q_OBJECT // 因为用到了自定义槽函数与信号槽连接

public:
    explicit DeviceSimulator(const aegis::core::DeviceConfig& config,
                             QObject* parent = nullptr)
        : QObject(parent)
        , config_(config)
        , channelCount_(config.channelCount)
        , sampleRateHz_(config.sampleRateHz)
        , rng_(42)              // 固定随机种子：每次运行噪声序列一致，便于复现
        , noise_(-1200, 1200)   // 均匀分布区间
    {
        // 有新客户端接入
        connect(&server_, &QTcpServer::newConnection, this, &DeviceSimulator::onNewConnection);
        // 定时产生数据
        connect(&timer_, &QTimer::timeout, this, &DeviceSimulator::onTick);

        // Modbus TCP 服务端（端口 5020）
        connect(&modbusServer_, &QTcpServer::newConnection,
                this, &DeviceSimulator::onModbusConnection);

        // SCPI 仪器服务端（端口 5025，SCPI-RAW over TCP）
        connect(&scpiServer_, &QTcpServer::newConnection,
                this, &DeviceSimulator::onScpiConnection);

        // PreciseTimer：Windows 上会使用更高精度的计时器，减少 10ms 周期抖动。
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setInterval(kTickMs);
    }

    // 开始监听：自定义协议、Modbus TCP、OPC UA、SCPI（端口为 0 表示不启动该服务）。
    bool start(quint16 port, quint16 modbusPort, quint16 opcuaPort, quint16 scpiPort)
    {
        modbusPort_ = modbusPort;
        opcuaPort_ = opcuaPort;
        scpiPort_ = scpiPort;

        const bool customOk = server_.listen(QHostAddress::Any, port);
        if (modbusPort_ != 0) {
            if (modbusServer_.listen(QHostAddress::Any, modbusPort_))
                qInfo("Modbus TCP 服务已启动: 0.0.0.0:%d", modbusPort_);
            else
                qWarning("Modbus TCP 端口 %d 监听失败", modbusPort_);
        }
        if (scpiPort_ != 0) {
            if (scpiServer_.listen(QHostAddress::Any, scpiPort_))
                qInfo("SCPI 服务已启动: 0.0.0.0:%d", scpiPort_);
            else
                qWarning("SCPI 端口 %d 监听失败", scpiPort_);
        }

#ifdef AEGIS_HAS_OPCUA
        if (opcuaPort_ != 0) {
            startOpcUa(opcuaPort_);
            // OPC UA 的值在 onTick 中刷新；主动启动定时器，保证"只连 OPC UA"时也持续变化。
            if (!timer_.isActive())
                timer_.start();
        }
#endif
        return customOk;
    }

    ~DeviceSimulator() override
    {
#ifdef AEGIS_HAS_OPCUA
        uaTimer_.stop();
        if (uaServer_ != nullptr) {
            UA_Server_run_shutdown(uaServer_);
            UA_Server_delete(uaServer_);
            uaServer_ = nullptr;
        }
        // 节点 ID 是 ALLOC 出来的字符串，需要逐个释放。
        for (UA_NodeId& node : uaNodes_)
            UA_NodeId_clear(&node);
        uaNodes_.clear();
#endif
    }

private slots:
    // =========================================================================
    // 处理新连接：保存客户端，并为其建立独立的解码器与收发逻辑
    // =========================================================================
    void onNewConnection()
    {
        // nextPendingConnection 可能有多个（同一时刻多个客户端接入），所以用 while。
        while (QTcpSocket* socket = server_.nextPendingConnection()) {
            // 每个连接一个解码器：不同连接的数据流是独立的，不能共用一个缓冲。
            auto* codec = new FrameCodec();
            codecs_.insert(socket, codec);

            // 收到客户端数据：解析帧，处理心跳与命令。
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, codec] {
                const QByteArray data = socket->readAll();
                codec->feed(reinterpret_cast<const std::uint8_t*>(data.constData()),
                            static_cast<std::size_t>(data.size()),
                            [this, socket](const Frame& frame) {
                                // 心跳：回一个成功应答，用于检查链路。
                                if (frame.type == FrameType::Ping) {
                                    AckPayload ack;
                                    ack.command = 0x00;
                                    ack.status = 0;
                                    ack.measured = 0;
                                    writeFrame(socket, FrameType::Ack, frame.seq,
                                               encodeAckPayload(ack));
                                    return;
                                }

                                if (frame.type != FrameType::Command)
                                    return;

                                // 命令帧：解析后修改模拟器设定值，再回 Ack。
                                CommandPayload command;
                                if (!decodeCommandPayload(frame.payload, command))
                                    return;

                                AckPayload ack;
                                ack.command = static_cast<std::uint8_t>(command.command);
                                ack.status = 0;
                                ack.measured = 0;

                                switch (command.command) {
                                case CommandId::SetVout:
                                    // 合理范围 0~15000 mV，越界返回状态 1。
                                    if (command.value < 0 || command.value > 15000)
                                        ack.status = 1;
                                    else
                                        voutMv_ = command.value;
                                    break;

                                case CommandId::SetIout:
                                    // 合理范围 0~5000 mA。
                                    if (command.value < 0 || command.value > 5000)
                                        ack.status = 1;
                                    else
                                        ioutMa_ = command.value;
                                    break;

                                case CommandId::ReadMeasure:
                                    ack.measured = readChannel(command.channel);
                                    break;

                                case CommandId::Reset:
                                    voutMv_ = 5000;
                                    ioutMa_ = 1000;
                                    break;

                                case CommandId::ReadVersion: {
                                    // 版本号以 Info 帧回同一序号，文本与 SCPI 的 *IDN? 保持一致。
                                    const QByteArray text =
                                        QByteArrayLiteral("AegisSim,DMM,0001,1.0");
                                    const std::vector<std::uint8_t> payload(
                                        text.begin(), text.end());
                                    writeFrame(socket, FrameType::Info, frame.seq, payload);
                                    qInfo("读取版本 -> %s", text.constData());
                                    return;
                                }
                                }

                                writeFrame(socket, FrameType::Ack, frame.seq,
                                           encodeAckPayload(ack));
                                qInfo("命令 cmd=%d ch=%d value=%d -> status=%d",
                                      static_cast<int>(command.command),
                                      static_cast<int>(command.channel),
                                      static_cast<int>(command.value),
                                      static_cast<int>(ack.status));
                            });
            });

            // 客户端断开：从列表移除、释放解码器、延迟删除 socket。
            // deleteLater 比 delete 安全：避免在信号处理过程中销毁信号发送者。
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                if (clients_.removeAll(socket) > 0) {
                    delete codecs_.take(socket);
                    qInfo("客户端断开，当前在线 %lld",
                          static_cast<long long>(clients_.size()));
                }
                socket->deleteLater();
            });

            clients_.append(socket);
            qInfo("客户端接入 %s", qPrintable(socket->peerAddress().toString()));

            // 第一个客户端接入时才启动数据产生；没有客户端时保持空闲，节省 CPU。
            if (!timer_.isActive())
                timer_.start();
        }
    }

    // =========================================================================
    // 每个周期产生一批波形数据，编码成帧，广播给所有客户端
    // =========================================================================
    void onTick()
    {
        // 先剔除已断开的 socket：某些异常断开（进程被杀、网络中断）不一定及时触发
        // disconnected 信号，若不清理会让"在线客户端"一直显示旧值。
        pruneDisconnectedClients();

        // interleaved：交叠排列的采样值，长度 = 100 点 × 通道数。
        const int channels = channelCount_;
        std::vector<std::int16_t> interleaved;
        interleaved.reserve(static_cast<std::size_t>(kSamplesPerTick) * channels);

        for (int i = 0; i < kSamplesPerTick; ++i) {
            // elapsedSec_ 是本批数据的起始时间，i/采样率 是批内偏移。
            const double t = elapsedSec_ + static_cast<double>(i) / sampleRateHz_;

            for (int ch = 0; ch < channels; ++ch) {
                double value = 0.0;
                if (ch == 0) {
                    // CH0：输出电压（mV）—— DC 值由 SET_VOUT 决定，叠加 ±20mV 纹波
                    value = voutMv_ + std::sin(2.0 * kPi * 50.0 * t) * 20.0;
                } else if (ch == 1) {
                    // CH1：输出电流（mA）—— DC 值由 SET_IOUT 决定
                    value = ioutMa_ + std::sin(2.0 * kPi * 10.0 * t) * 5.0;
                } else {
                    // 其余通道按序号轮换波形，通道数增加时自动扩展。
                    value = simulateChannel(ch, t);
                }
                interleaved.push_back(clamp16(value));
            }
        }

        // 组帧：类型 DATA，序号自增（uint16 溢出后回绕，接收方可据此发现断流）。
        Frame frame;
        frame.type = FrameType::Data;
        frame.seq = seq_++;
        frame.payload = encodeDataPayload(static_cast<std::uint32_t>(elapsedSec_ * 1000.0),
                                          interleaved.data(), interleaved.size());

        // 编码一次，广播给所有客户端（编码结果与具体连接无关，可复用）。
        const auto bytes = FrameCodec::encode(frame);
        for (auto* socket : clients_) {
            if (socket->state() == QAbstractSocket::ConnectedState) {
                socket->write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<qint64>(bytes.size()));
            }
        }

        // 推进本批数据的时间起点：100 个点 / 采样率。
        elapsedSec_ += static_cast<double>(kSamplesPerTick) / sampleRateHz_;

#ifdef AEGIS_HAS_OPCUA
        updateOpcUa();
#endif

        // 每 100 个周期（约 1 秒）打印一次心跳日志。
        if (++tickCount_ % 100 == 0) {
            qInfo("已发送 %llu 帧, 在线客户端 %lld",
                  static_cast<unsigned long long>(seq_),
                  static_cast<long long>(clients_.size()));
        }
    }

    // 生成非 CH0/CH1 的模拟波形：按通道序号轮换方波/三角波/噪声/锯齿。
    // 频率随通道号缓慢变化，便于在 32/64 通道时区分不同曲线。
    double simulateChannel(int channel, double t)
    {
        switch (channel % 4) {
        case 0: {
            const double square =
                std::fmod(t * (5.0 + 0.1 * channel), 1.0) < 0.5 ? 1.0 : -1.0;
            return square * 6000.0;
        }
        case 1: {
            const double phase = std::fmod(t * (3.0 + 0.05 * channel), 1.0);
            return (4.0 * std::fabs(phase - 0.5) - 1.0) * 9000.0;
        }
        case 2:
            return static_cast<double>(noise_(rng_)); // 传感器本底噪声
        default: {
            const double phase = std::fmod(t * (1.0 + 0.02 * channel), 1.0);
            return (phase * 2.0 - 1.0) * 7000.0;
        }
        }
    }

    // =========================================================================
    // Modbus TCP：接受连接并逐帧解析请求
    // =========================================================================
    void onModbusConnection()
    {
        while (QTcpSocket* socket = modbusServer_.nextPendingConnection()) {
            modbusBuffers_.insert(socket, QByteArray());

            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                QByteArray& buffer = modbusBuffers_[socket];
                buffer.append(socket->readAll());

                while (true) {
                    const auto* data =
                        reinterpret_cast<const std::uint8_t*>(buffer.constData());
                    const std::size_t length = modbus::tcpFrameLength(
                        data, static_cast<std::size_t>(buffer.size()));
                    if (length == 0)
                        break; // 半包，等后续数据

                    const QByteArray frame = buffer.left(static_cast<int>(length));
                    buffer.remove(0, static_cast<int>(length));
                    handleModbusRequest(socket, frame);
                }
            });

            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                modbusBuffers_.remove(socket);
                socket->deleteLater();
            });

            qInfo("Modbus 客户端接入 %s", qPrintable(socket->peerAddress().toString()));
        }
    }

    // =========================================================================
    // SCPI 仪器：按行解析命令（*IDN? / MEAS:VOLT:DC? / SOUR:VOLT x / *RST）
    // =========================================================================
    void onScpiConnection()
    {
        while (QTcpSocket* socket = scpiServer_.nextPendingConnection()) {
            scpiBuffers_.insert(socket, QByteArray());

            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                QByteArray& buffer = scpiBuffers_[socket];
                buffer.append(socket->readAll());

                while (true) {
                    const int pos = buffer.indexOf('\n');
                    if (pos < 0)
                        break;
                    const QByteArray line = buffer.left(pos);
                    buffer.remove(0, pos + 1);
                    handleScpiCommand(socket, line);
                }
            });

            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                scpiBuffers_.remove(socket);
                socket->deleteLater();
            });

            qInfo("SCPI 客户端接入 %s", qPrintable(socket->peerAddress().toString()));
        }
    }

    // 处理一条 SCPI 命令；查询类命令（以 ? 结尾）回一行文本。
    void handleScpiCommand(QTcpSocket* socket, const QByteArray& line)
    {
        const QString command = QString::fromLatin1(line).trimmed();
        if (command.isEmpty())
            return;
        const QString upper = command.toUpper();

        QString response;
        if (upper == QLatin1String("*IDN?")) {
            response = QStringLiteral("AegisSim,DMM,0001,1.0");
        } else if (upper.startsWith(QLatin1String("*RST"))) {
            voutMv_ = 5000;
            ioutMa_ = 1000;
        } else if (upper.startsWith(QLatin1String("MEAS:VOLT"))) {
            response = QString::number(voutMv_);
        } else if (upper.startsWith(QLatin1String("MEAS:CURR"))) {
            response = QString::number(ioutMa_);
        } else if (upper.startsWith(QLatin1String("SOUR:VOLT"))) {
            voutMv_ = clamp16(command.section(QLatin1Char(' '), 1, 1).toDouble() * 1000.0);
        } else if (upper.startsWith(QLatin1String("SOUR:CURR"))) {
            ioutMa_ = clamp16(command.section(QLatin1Char(' '), 1, 1).toDouble() * 1000.0);
        } else if (upper == QLatin1String("*OPC?") || upper.endsWith(QLatin1Char('?'))) {
            response = QStringLiteral("0");
        }

        if (!response.isEmpty())
            socket->write(response.toLatin1() + '\n');
    }

private:
    // 处理一条完整的 Modbus TCP 请求并回响应。
    //   0x03/0x04：读寄存器，0=电压 1=电流 2=温度 3~5=预留
    //   0x06：写单个寄存器（0 设电压、1 设电流）
    void handleModbusRequest(QTcpSocket* socket, const QByteArray& frame)
    {
        if (frame.size() < 8)
            return;

        const auto* d = reinterpret_cast<const std::uint8_t*>(frame.constData());
        const std::uint16_t transactionId = static_cast<std::uint16_t>((d[0] << 8) | d[1]);
        const std::uint8_t unitId = d[6];
        const std::uint8_t function = d[7];

        QByteArray response;
        auto appendU16 = [&response](std::uint16_t value) {
            response.append(static_cast<char>((value >> 8) & 0xFF));
            response.append(static_cast<char>(value & 0xFF));
        };

        // MBAP：事务号 + 协议号 + 长度（先占位）+ 单元号
        appendU16(transactionId);
        appendU16(0);
        response.append(static_cast<char>(0));
        response.append(static_cast<char>(0));
        response.append(static_cast<char>(unitId));

        if (function == 0x03 || function == 0x04) {
            if (frame.size() < 12)
                return;

            const std::uint16_t start = static_cast<std::uint16_t>((d[8] << 8) | d[9]);
            const std::uint16_t count = static_cast<std::uint16_t>((d[10] << 8) | d[11]);

            const std::uint16_t registers[6] = {
                static_cast<std::uint16_t>(voutMv_),
                static_cast<std::uint16_t>(ioutMa_),
                250, 0, 0, 0,
            };

            // 版本寄存器区：0x0100 起 12 个（24 字节），大端 ASCII，NUL 填充。
            static constexpr std::uint16_t kVersionBase = 0x0100;
            static constexpr std::uint16_t kVersionCount = 12;
            static const QByteArray kVersion = QByteArrayLiteral("AegisSim,DMM,0001,1.0");

            QByteArray dataBytes;
            std::uint16_t valid = 0;
            if (start >= kVersionBase && start < kVersionBase + kVersionCount) {
                for (std::uint16_t i = 0; i < count; ++i) {
                    const int regIndex = static_cast<int>(start) + i - kVersionBase;
                    if (regIndex < 0 || regIndex >= kVersionCount)
                        break;
                    const int byteIndex = regIndex * 2;
                    const std::uint8_t hi =
                        byteIndex < kVersion.size()
                            ? static_cast<std::uint8_t>(kVersion.at(byteIndex)) : 0;
                    const std::uint8_t lo =
                        byteIndex + 1 < kVersion.size()
                            ? static_cast<std::uint8_t>(kVersion.at(byteIndex + 1)) : 0;
                    dataBytes.append(static_cast<char>(hi));
                    dataBytes.append(static_cast<char>(lo));
                    ++valid;
                }
            } else {
                for (std::uint16_t i = 0; i < count && start + i < 6; ++i) {
                    const std::uint16_t value = registers[start + i];
                    dataBytes.append(static_cast<char>((value >> 8) & 0xFF));
                    dataBytes.append(static_cast<char>(value & 0xFF));
                    ++valid;
                }
            }

            response.append(static_cast<char>(function));
            response.append(static_cast<char>(valid * 2));
            response.append(dataBytes);
        } 
        else if (function == 0x06) 
        {
            if (frame.size() < 12)
                return;

            const std::uint16_t address = static_cast<std::uint16_t>((d[8] << 8) | d[9]);
            const std::uint16_t value = static_cast<std::uint16_t>((d[10] << 8) | d[11]);

            if (address == 0)
                voutMv_ = static_cast<std::int16_t>(value);
            else if (address == 1)
                ioutMa_ = static_cast<std::int16_t>(value);

            // 写单个寄存器的响应就是请求回显。
            response.append(static_cast<char>(function));
            appendU16(address);
            appendU16(value);
        } else {
            // 不支持的功能码 -> 异常响应
            response.append(static_cast<char>(function | 0x80));
            response.append(static_cast<char>(0x01));
        }

        // 回填 MBAP 长度字段（不含前 6 字节）
        const auto length = static_cast<std::uint16_t>(response.size() - 6);
        response[4] = static_cast<char>((length >> 8) & 0xFF);
        response[5] = static_cast<char>(length & 0xFF);

        socket->write(response);
    }

#ifdef AEGIS_HAS_OPCUA
    // 启动一个最小 OPC UA 服务端：命名空间 1 下暴露 Channel0..ChannelN-1。
    void startOpcUa(quint16 port)
    {
        uaServer_ = UA_Server_new();
        // setMinimal 指定监听端口（默认 4840），并启用匿名 + SecurityPolicyNone。
        UA_ServerConfig_setMinimal(UA_Server_getConfig(uaServer_), port, nullptr);

        uaNodes_.assign(static_cast<std::size_t>(channelCount_), UA_NODEID_NULL);
        uaLastWritten_.assign(static_cast<std::size_t>(channelCount_), 0.0);

        // 初始值与当前设定值一致，避免上位机刚连上就读到 0。
        std::vector<double> initial(static_cast<std::size_t>(channelCount_), 0.0);
        if (channelCount_ > 0)
            initial[0] = static_cast<double>(voutMv_);
        if (channelCount_ > 1)
            initial[1] = static_cast<double>(ioutMa_);
        if (channelCount_ > 2)
            initial[2] = 250.0;
        uaLastWritten_ = initial;

        for (int i = 0; i < channelCount_; ++i) {
            const QByteArray name = QStringLiteral("Channel%1").arg(i).toUtf8();

            UA_VariableAttributes attributes = UA_VariableAttributes_default;
            UA_Variant_setScalar(&attributes.value, &initial[static_cast<std::size_t>(i)],
                                 &UA_TYPES[UA_TYPES_DOUBLE]);
            attributes.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
            // 节点 0/1 是可写设定值，其余为只读测量值。
            attributes.accessLevel = (i < 2)
                ? (UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE)
                : UA_ACCESSLEVELMASK_READ;
            attributes.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"),
                                                      const_cast<char*>(name.constData()));

            // 用 ALLOC 版本保存节点 ID 副本（函数返回后 name 就失效了）。
            uaNodes_[static_cast<std::size_t>(i)] =
                UA_NODEID_STRING_ALLOC(1, name.constData());

            UA_Server_addVariableNode(
                uaServer_, uaNodes_[static_cast<std::size_t>(i)],
                UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                UA_QUALIFIEDNAME(1, const_cast<char*>(name.constData())),
                UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                attributes, nullptr, nullptr);
        }

        // 固件版本节点：ns=1;s=FirmwareVersion（String，只读），与 SCPI *IDN? 返回一致。
        {
            UA_String version = UA_STRING_ALLOC("AegisSim,DMM,0001,1.0");

            UA_VariableAttributes attributes = UA_VariableAttributes_default;
            UA_Variant_setScalar(&attributes.value, &version, &UA_TYPES[UA_TYPES_STRING]);
            attributes.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
            attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
            attributes.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"),
                                                      const_cast<char*>("FirmwareVersion"));

            const UA_StatusCode status = UA_Server_addVariableNode(
                uaServer_, UA_NODEID_STRING(1, const_cast<char*>("FirmwareVersion")),
                UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                UA_QUALIFIEDNAME(1, const_cast<char*>("FirmwareVersion")),
                UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                attributes, nullptr, nullptr);
            UA_String_clear(&version);

            if (status != UA_STATUSCODE_GOOD)
                qWarning("OPC UA 版本节点创建失败: %s", UA_StatusCode_name(status));
        }

        UA_Server_run_startup(uaServer_);
        uaTimer_.setInterval(10);
        connect(&uaTimer_, &QTimer::timeout, this, [this] {
            UA_Server_run_iterate(uaServer_, false);
        });
        uaTimer_.start();
        qInfo("OPC UA 服务已启动: opc.tcp://0.0.0.0:%d（%d 通道）", port, channelCount_);
    }

    // 把当前设定值写入 OPC UA 变量；同时读回可写节点，接受客户端写入的新设定值。
    void updateOpcUa()
    {
        if (uaServer_ == nullptr || channelCount_ <= 0)
            return;

        // 节点 0/1 可写：服务端当前值若与"上次写出的值"不同，说明客户端写了新值。
        // 用比较而不是读后直接覆盖，是为了不把自定义协议刚设置的设定值改回去。
        for (int i = 0; i < std::min(2, channelCount_); ++i) {
            UA_Variant variant;
            UA_Variant_init(&variant);
            const std::size_t index = static_cast<std::size_t>(i);
            if (UA_Server_readValue(uaServer_, uaNodes_[index], &variant)
                    == UA_STATUSCODE_GOOD
                && UA_Variant_hasScalarType(&variant, &UA_TYPES[UA_TYPES_DOUBLE])) {
                const double current = *static_cast<UA_Double*>(variant.data);
                if (std::fabs(current - uaLastWritten_[index]) > 0.5) {
                    const auto value = static_cast<std::int16_t>(
                        std::clamp(current, -32768.0, 32767.0));
                    if (i == 0)
                        voutMv_ = value;
                    else
                        ioutMa_ = value;
                    qInfo("OPC UA 写入设定值: %s=%d",
                          i == 0 ? "VOUT(mV)" : "IOUT(mA)",
                          static_cast<int>(value));
                }
            }
            UA_Variant_clear(&variant);
        }

        std::vector<double> values(static_cast<std::size_t>(channelCount_), 0.0);
        if (channelCount_ > 0)
            values[0] = static_cast<double>(voutMv_);
        if (channelCount_ > 1)
            values[1] = static_cast<double>(ioutMa_);
        if (channelCount_ > 2)
            values[2] = 250.0;

        for (int i = 0; i < channelCount_; ++i) {
            const std::size_t index = static_cast<std::size_t>(i);
            UA_Variant variant;
            UA_Variant_init(&variant);
            UA_Variant_setScalar(&variant, &values[index], &UA_TYPES[UA_TYPES_DOUBLE]);
            UA_Server_writeValue(uaServer_, uaNodes_[index], variant);
            uaLastWritten_[index] = values[index];
        }
    }
#endif

    // 清理状态已不是 Connected 的客户端（异常断开的兜底）。
    // 正常情况下 disconnected 信号会先处理；这里防止进程被杀/网络异常时列表残留。
    void pruneDisconnectedClients()
    {
        for (int i = clients_.size() - 1; i >= 0; --i) {
            QTcpSocket* socket = clients_.at(i);
            if (socket == nullptr
                || socket->state() == QAbstractSocket::ConnectedState) {
                continue;
            }

            clients_.removeAt(i);
            socket->disconnect(this); // 先断开信号，避免删掉 codec 后仍回调 readyRead
            delete codecs_.take(socket);
            socket->deleteLater();
            qInfo("清理已断开的客户端，当前在线 %lld",
                  static_cast<long long>(clients_.size()));
        }
    }

    // 统一发送一个帧：所有应答都走这里，避免重复代码。
    void writeFrame(QTcpSocket* socket, FrameType type, std::uint16_t seq,
                    const std::vector<std::uint8_t>& payload)    {
        Frame frame;
        frame.type = type;
        frame.seq = seq;
        frame.payload = payload;

        const auto bytes = FrameCodec::encode(frame);
        if (socket != nullptr && socket->state() == QAbstractSocket::ConnectedState) {
            socket->write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<qint64>(bytes.size()));
        }
    }

    // 读取某通道的当前值：DC 通道返回设定值，其余通道没有实际意义返回 0。
    std::int16_t readChannel(int channel) const
    {
        switch (channel) {
        case 0:
            return voutMv_;
        case 1:
            return ioutMa_;
        default:
            return 0;
        }
    }

    aegis::core::DeviceConfig config_; // 通道数/采样率/通道名
    int channelCount_ = aegis::core::DeviceConfig::kDefaultChannelCount;
    int sampleRateHz_ = aegis::core::DeviceConfig::kDefaultSampleRateHz;
    quint16 modbusPort_ = 0;           // 0 表示不启用 Modbus TCP
    quint16 opcuaPort_ = 0;            // 0 表示不启用 OPC UA
    quint16 scpiPort_ = 0;             // 0 表示不启用 SCPI

    QTcpServer server_;                  // 自定义协议监听器
    QTcpServer modbusServer_;            // Modbus TCP 监听器
    QHash<QTcpSocket*, QByteArray> modbusBuffers_; // Modbus 连接的接收缓冲
    QTcpServer scpiServer_;              // SCPI 监听器
    QHash<QTcpSocket*, QByteArray> scpiBuffers_; // SCPI 连接的接收缓冲
    QTimer timer_;                       // 数据产生定时器
    QList<QTcpSocket*> clients_;         // 当前在线的客户端
    QHash<QTcpSocket*, FrameCodec*> codecs_; // 每个客户端对应的解码器
    std::mt19937 rng_;                   // 伪随机数引擎
    std::uniform_int_distribution<int> noise_; // 噪声分布
    double elapsedSec_ = 0.0;            // 模拟运行的累计时间（秒）
    std::uint16_t seq_ = 0;              // 帧序号
    int tickCount_ = 0;                  // 定时器触发次数（用于降低日志频率）
    std::int16_t voutMv_ = 5000;         // 当前设定的输出电压（mV）
    std::int16_t ioutMa_ = 1000;         // 当前设定的输出电流（mA）

#ifdef AEGIS_HAS_OPCUA
    UA_Server* uaServer_ = nullptr;              // OPC UA 服务端
    QTimer uaTimer_;                             // 驱动 UA 事件循环
    std::vector<UA_NodeId> uaNodes_;             // 各通道变量节点
    // 上一次写入节点的值：用于识别客户端写操作（与当前节点值不同即视为被改写）。
    std::vector<double> uaLastWritten_;
#endif
};

// =============================================================================
// 入口：控制台程序，启动模拟器后进入事件循环
// =============================================================================
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv); // 非 GUI 应用用 QCoreApplication（更轻量）

    // 支持多进程多设备演示：每个进程用不同端口/通道配置。
    //   aegis_sim.exe --port 9528 --modbus-port 5021 --opcua-port 4841 --config xxx.json
    quint16 port = 9527;
    quint16 modbusPort = 5020; // 502 需管理员权限，演示默认 5020
    quint16 opcuaPort = 4840;
    quint16 scpiPort = 5025; // SCPI-RAW 标准端口
    QString configPath;

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        const auto nextValue = [&args, &i] {
            return (i + 1 < args.size()) ? args.at(++i) : QString();
        };
        if (arg == QLatin1String("--port"))
            port = static_cast<quint16>(nextValue().toUShort());
        else if (arg == QLatin1String("--modbus-port"))
            modbusPort = static_cast<quint16>(nextValue().toUShort());
        else if (arg == QLatin1String("--opcua-port"))
            opcuaPort = static_cast<quint16>(nextValue().toUShort());
        else if (arg == QLatin1String("--scpi-port"))
            scpiPort = static_cast<quint16>(nextValue().toUShort());
        else if (arg == QLatin1String("--config"))
            configPath = nextValue();
    }

    aegis::core::DeviceConfig config;
    if (!configPath.isEmpty()) {
        QString error;
        config = aegis::core::DeviceConfig::load(configPath, &error);
        if (!error.isEmpty())
            qWarning("读取通道配置失败：%s（使用默认值）", qUtf8Printable(error));
    } else {
        config = aegis::core::DeviceConfig::loadActive();
    }

    DeviceSimulator simulator(config);
    if (!simulator.start(port, modbusPort, opcuaPort, scpiPort)) {
        qCritical("端口 %d 监听失败", port);
        return 1; // 非 0 退出码表示异常结束
    }

    qInfo("DeviceSimulator 已启动: 0.0.0.0:%d, %d 通道 @ %d Hz", port,
          config.channelCount, config.sampleRateHz);

    return app.exec();
}

// Q_OBJECT 写在 .cpp 里时，需要包含 moc 生成的代码，否则链接会缺信号槽实现。
#include "main.moc"
