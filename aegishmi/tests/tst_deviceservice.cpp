// Qt Test 框架
#include <QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

#include "protocol/frame.h"
#include "protocol/frame_codec.h"
#include "protocol/modbus.h"
#include "service/alarm_monitor.h"
#include "service/device_service.h"
#include "service/device_manager.h"
#include "storage/system_repository.h"
#include "test/test_engine.h"

using namespace aegis;

namespace {

// 在事件循环里轮询等待条件成立；期间保持主线程能处理网络事件。
bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(10);
    return predicate();
}

// =============================================================================
// FakeDeviceServer —— 进程内的最小假设备
//
// 行为：
//   - 每 10ms 推一帧 DATA（100 点 × N 通道，默认 6 通道）
//   - 收到 Command 立即回 Ack（status=0）
// 这样可以在没有真实模拟器进程的情况下验证 DeviceService 的收/发链路。
// =============================================================================
class FakeDeviceServer : public QObject {
    Q_OBJECT

public:
    explicit FakeDeviceServer(int channelCount = protocol::kDefaultChannelCount,
                              QObject* parent = nullptr)
        : QObject(parent)
        , channelCount_(channelCount)
    {
        server_.listen(QHostAddress::LocalHost, 0); // 端口 0：由系统分配
        connect(&server_, &QTcpServer::newConnection, this, &FakeDeviceServer::onNewConnection);

        timer_.setInterval(10);
        // 必须用高精度定时器：Windows 默认粗定时器精度约 15.6ms，10ms 周期会退化成 16ms，
        // 导致 100ms 窗口只收到 600 点而不是 1000 点。
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, &FakeDeviceServer::sendData);
        timer_.start();
    }

    quint16 port() const { return server_.serverPort(); }

    // 主动断开全部客户端（服务端仍在监听），用于验证自动重连。
    void dropClients()
    {
        for (QTcpSocket* socket : clients_) {
            socket->disconnectFromHost();
            socket->deleteLater();
        }
        clients_.clear();
        qDeleteAll(codecs_);
        codecs_.clear();
    }

private slots:
    void onNewConnection()
    {
        while (QTcpSocket* socket = server_.nextPendingConnection()) {
            clients_.append(socket);
            auto* codec = new protocol::FrameCodec();
            codecs_.insert(socket, codec);

            connect(socket, &QTcpSocket::readyRead, this, [this, socket, codec] {
                const QByteArray data = socket->readAll();
                codec->feed(reinterpret_cast<const std::uint8_t*>(data.constData()),
                            static_cast<std::size_t>(data.size()),
                            [socket](const protocol::Frame& frame) {
                                if (frame.type != protocol::FrameType::Command)
                                    return;

                                const quint8 command =
                                    frame.payload.empty() ? 0 : frame.payload[0];

                                protocol::Frame reply;
                                if (command
                                    == static_cast<quint8>(
                                        protocol::CommandId::ReadVersion)) {
                                    // 版本查询：Info 帧回同一序号的文本。
                                    const QByteArray text("AegisSim,DMM,0001,1.0");
                                    reply.type = protocol::FrameType::Info;
                                    reply.seq = frame.seq;
                                    reply.payload.assign(text.begin(), text.end());
                                } else {
                                    protocol::AckPayload ack;
                                    ack.command = command;
                                    ack.status = 0;
                                    reply.type = protocol::FrameType::Ack;
                                    reply.seq = frame.seq;
                                    reply.payload = protocol::encodeAckPayload(ack);
                                }

                                const auto bytes = protocol::FrameCodec::encode(reply);
                                socket->write(reinterpret_cast<const char*>(bytes.data()),
                                              static_cast<qint64>(bytes.size()));
                            });
            });
        }
    }

    void sendData()
    {
        if (clients_.isEmpty())
            return;

        // 每次发 200 个点（等效 20kHz）。
        // 取 20kHz 而不是 10kHz 是为了让测试对 Windows 定时器抖动不敏感：
        // 即使实际周期退化到 20ms，100ms 窗口仍能收到约 1000 点，满足引擎 80% 的阈值。
        constexpr int kSamplesPerTick = 200;

        std::vector<std::int16_t> interleaved;
        interleaved.reserve(static_cast<std::size_t>(kSamplesPerTick) * channelCount_);
        for (int i = 0; i < kSamplesPerTick; ++i) {
            for (int ch = 0; ch < channelCount_; ++ch) {
                // CH0 电压、CH1 电流，其余通道固定 0（测试只关心数量与通道映射）。
                const std::int16_t value =
                    ch == 0 ? static_cast<std::int16_t>(5000)
                            : (ch == 1 ? static_cast<std::int16_t>(1000) : 0);
                interleaved.push_back(value);
            }
        }

        protocol::Frame frame;
        frame.type = protocol::FrameType::Data;
        frame.seq = seq_++;
        frame.payload = protocol::encodeDataPayload(timestampMs_, interleaved.data(),
                                                    interleaved.size());
        timestampMs_ += 10;

        const auto bytes = protocol::FrameCodec::encode(frame);
        for (auto* socket : clients_) {
            if (socket->state() == QAbstractSocket::ConnectedState) {
                socket->write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<qint64>(bytes.size()));
            }
        }
    }

private:
    QTcpServer server_;
    QTimer timer_;
    QList<QTcpSocket*> clients_;
    QHash<QTcpSocket*, protocol::FrameCodec*> codecs_;
    int channelCount_ = protocol::kDefaultChannelCount;
    std::uint16_t seq_ = 0;
    std::uint32_t timestampMs_ = 0;
};

// =============================================================================
// FakeSilentServer —— 只接受连接、不回任何数据的服务端
//
// 用于验证链路心跳：上位机应主动发 Ping，超过判定阈值后自行断开并重连。
// =============================================================================
class FakeSilentServer : public QObject {
    Q_OBJECT

public:
    explicit FakeSilentServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        server_.listen(QHostAddress::LocalHost, 0);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    // 只有心跳 Ping 会到达这里（无命令、无数据）。
                    if (!socket->readAll().isEmpty())
                        ++pingCount_;
                });
            }
        });
    }

    quint16 port() const { return server_.serverPort(); }
    int pingCount() const { return pingCount_; }

private:
    QTcpServer server_;
    int pingCount_ = 0;
};

} // namespace

// =============================================================================
// FakeModbusServer —— 进程内的最小 Modbus TCP 从站
// 对任意读请求固定返回 {5000, 1000, 250, 0, 0, 0}。
// =============================================================================
class FakeModbusServer : public QObject {
    Q_OBJECT

public:
    explicit FakeModbusServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        server_.listen(QHostAddress::LocalHost, 0);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                buffers_.insert(socket, QByteArray());

                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray& buffer = buffers_[socket];
                    buffer.append(socket->readAll());

                    while (true) {
                        const auto* data =
                            reinterpret_cast<const std::uint8_t*>(buffer.constData());
                        const std::size_t length = protocol::modbus::tcpFrameLength(
                            data, static_cast<std::size_t>(buffer.size()));
                        if (length == 0)
                            break;

                        const QByteArray request = buffer.left(static_cast<int>(length));
                        buffer.remove(0, static_cast<int>(length));
                        reply(socket, request);
                    }
                });
            }
        });
    }

    quint16 port() const { return server_.serverPort(); }
    quint16 lastWrittenValue() const { return lastWrittenValue_; }

private:
    void reply(QTcpSocket* socket, const QByteArray& request)
    {
        const auto* d = reinterpret_cast<const std::uint8_t*>(request.constData());
        const std::uint16_t transactionId =
            static_cast<std::uint16_t>((d[0] << 8) | d[1]);
        const std::uint8_t unitId = d[6];
        const std::uint8_t function = d[7];

        QByteArray response;
        response.append(static_cast<char>((transactionId >> 8) & 0xFF));
        response.append(static_cast<char>(transactionId & 0xFF));
        response.append(static_cast<char>(0)); // 协议号
        response.append(static_cast<char>(0));
        response.append(static_cast<char>(0)); // 长度占位
        response.append(static_cast<char>(0));
        response.append(static_cast<char>(unitId));

        if (function == 0x06) {
            // 写单个寄存器：记录值并原样回显
            const std::uint16_t address =
                static_cast<std::uint16_t>((d[8] << 8) | d[9]);
            const std::uint16_t value =
                static_cast<std::uint16_t>((d[10] << 8) | d[11]);
            if (address == 0)
                lastWrittenValue_ = value;

            response.append(static_cast<char>(function));
            response.append(static_cast<char>((address >> 8) & 0xFF));
            response.append(static_cast<char>(address & 0xFF));
            response.append(static_cast<char>((value >> 8) & 0xFF));
            response.append(static_cast<char>(value & 0xFF));
        } else if (function == 0x03
                   && static_cast<std::uint16_t>((d[8] << 8) | d[9]) == 0x0100) {
            // 版本寄存器区：0x0100 起 12 个 = 24 字节，ASCII 大端，NUL 填充
            const QByteArray version("AegisSim,DMM,0001,1.0");
            QByteArray dataBytes;
            dataBytes.reserve(24);
            for (int i = 0; i < 12; ++i) {
                const int byteIndex = i * 2;
                dataBytes.append(byteIndex < version.size() ? version.at(byteIndex) : '\0');
                dataBytes.append(byteIndex + 1 < version.size()
                                     ? version.at(byteIndex + 1) : '\0');
            }

            response.append(static_cast<char>(function));
            response.append(static_cast<char>(dataBytes.size()));
            response.append(dataBytes);
        } else {
            response.append(static_cast<char>(0x04)); // FC04
            response.append(static_cast<char>(12));   // 6 个寄存器 = 12 字节

            const std::uint16_t registers[6] = {5000, 1000, 250, 0, 0, 0};
            for (const auto value : registers) {
                response.append(static_cast<char>((value >> 8) & 0xFF));
                response.append(static_cast<char>(value & 0xFF));
            }
        }

        const auto length = static_cast<std::uint16_t>(response.size() - 6);
        response[4] = static_cast<char>((length >> 8) & 0xFF);
        response[5] = static_cast<char>(length & 0xFF);
        socket->write(response);
    }

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    quint16 lastWrittenValue_ = 0;
};

// =============================================================================
// TstDeviceService —— DeviceService 作为 ITestDevice 的真实链路测试
// =============================================================================
class TstDeviceService : public QObject {
    Q_OBJECT

private slots:
    void sendCommandReceivesAck();
    void queryTextReadsVersion();
    void sampleWindowCollectsData();
    void supportsConfiguredChannels();
    void managerRunsTwoDevices();
    void managerSupportsTwoLinks();
    void managerLinkHealthAndSamplingRoute();
    void loadsFleetWithLinksAndLegacy();
    void emitsSecondSummaries();
    void engineRunsFlowEndToEnd();
    void modbusPollerProducesSamples();
    void modbusWriteCommandUpdatesRegister();
    void queryTextReadsModbusVersion();
    void alarmMonitorRaisesAndClears();
    void alarmShelveSuppressesNewAlarms();
    void deviceConfigParsesCalibrationAndCheck();
    void autoReconnectsAfterDrop();
    void heartbeatDetectsSilentLink();
};

// 命令发送：工作线程阻塞等待，主线程处理网络事件并唤醒。
void TstDeviceService::sendCommandReceivesAck()
{
    FakeDeviceServer server;
    // DeviceService 内含分块缓冲（MB 级），不能放栈上，否则栈溢出。
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));

    std::atomic<bool> done{false};
    bool ok = false;
    quint8 status = 0xFF;

    // 模拟真实场景：引擎在工作线程里调用，主线程保持事件循环。
    std::thread worker([&] {
        ok = device->sendCommand(test::DeviceCommand::SetVout, 0, 6000, 500, status);
        done.store(true);
    });

    QVERIFY(waitUntil([&] { return done.load(); }));
    worker.join();

    QVERIFY(ok);
    QCOMPARE(static_cast<int>(status), 0);
}

// 版本读取：自定义协议 ReadVersion → Info 帧文本 → queryText 返回版本串。
void TstDeviceService::queryTextReadsVersion()
{
    FakeDeviceServer server;
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));

    std::atomic<bool> done{false};
    bool ok = false;
    QString value;
    QString error;

    std::thread worker([&] {
        ok = device->queryText(QString(), 1000, &value, &error);
        done.store(true);
    });

    QVERIFY(waitUntil([&] { return done.load(); }));
    worker.join();

    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(value, QStringLiteral("AegisSim,DMM,0001,1.0"));
}

// 采样窗口：100ms 应收到约 1000 个点（允许 20% 缺量，断言 >= 800）。
void TstDeviceService::sampleWindowCollectsData()
{
    FakeDeviceServer server;
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));
    QTest::qWait(100); // 先让数据流稳定

    std::atomic<bool> done{false};
    std::vector<std::int16_t> samples;

    std::thread worker([&] {
        samples = device->sampleWindow(0, 100);
        done.store(true);
    });

    QVERIFY(waitUntil([&] { return done.load(); }));
    worker.join();

    QVERIFY2(samples.size() >= 800,
             qPrintable(QStringLiteral("collected %1 samples").arg(samples.size())));
    // 假设备 CH0 恒定为 5000，取几个点检查数值正确。
    QCOMPARE(static_cast<int>(samples.front()), 5000);
    QCOMPARE(static_cast<int>(samples.back()), 5000);
}

// 通道配置：32 通道设备的数据应能正确进入分块缓冲，并按配置校验通道号。
void TstDeviceService::supportsConfiguredChannels()
{
    FakeDeviceServer server(32);
    auto device = std::make_unique<service::DeviceService>(
        nullptr, core::DeviceConfig::make(32, 10000));

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));
    QVERIFY(waitUntil([&] { return device->ring()->size() > 0; }));

    // 校验分块缓冲里的通道数与点数。
    int chunkChannels = 0;
    int chunkPoints = 0;
    device->ring()->consume([&](const core::ChunkView& view) {
        chunkChannels = view.channelCount;
        chunkPoints = view.pointCount;
    });
    QCOMPARE(chunkChannels, 32);
    QVERIFY(chunkPoints > 0);

    // 最后一个合法通道可采集。
    std::atomic<bool> done{false};
    std::vector<std::int16_t> samples;
    std::thread worker([&] {
        samples = device->sampleWindow(31, 100);
        done.store(true);
    });
    QVERIFY(waitUntil([&] { return done.load(); }));
    worker.join();
    QVERIFY(!samples.empty());

    // 超出配置的通道号应立即拒绝（不会等待窗口）。
    QVERIFY(device->sampleWindow(32, 10).empty());
}

// 多设备并发：两台假设备各自独立连接/缓冲，断开一台不影响另一台。
void TstDeviceService::managerRunsTwoDevices()
{
    FakeDeviceServer server1(6);
    FakeDeviceServer server2(32);

    auto manager = std::make_unique<service::DeviceManager>();

    service::DeviceSpec spec1;
    spec1.id = QStringLiteral("devA");
    spec1.name = QStringLiteral("A");
    spec1.host = QStringLiteral("127.0.0.1");
    spec1.port = server1.port();

    service::DeviceSpec spec2 = spec1;
    spec2.id = QStringLiteral("devB");
    spec2.name = QStringLiteral("B");
    spec2.port = server2.port();

    QVERIFY(manager->addDevice(spec1, core::DeviceConfig::make(6)));
    QVERIFY(manager->addDevice(spec2, core::DeviceConfig::make(32)));
    QVERIFY(!manager->addDevice(spec1, core::DeviceConfig::make(6))); // id 重复应失败

    manager->connectAll();
    QVERIFY(waitUntil([&] {
        return manager->device(QStringLiteral("devA"))->isConnected()
            && manager->device(QStringLiteral("devB"))->isConnected();
    }));

    // 两台设备各自收数（互不阻塞）。
    QVERIFY(waitUntil([&] {
        return manager->device(QStringLiteral("devA"))->ring()->size() > 0
            && manager->device(QStringLiteral("devB"))->ring()->size() > 0;
    }));

    // 断开 A，不影响 B。
    manager->disconnectDevice(QStringLiteral("devA"));
    QVERIFY(waitUntil([&] {
        return !manager->device(QStringLiteral("devA"))->isConnected();
    }));
    QVERIFY(manager->device(QStringLiteral("devB"))->isConnected());

    // 两台设备的通道配置互相独立。
    QCOMPARE(manager->device(QStringLiteral("devA"))->config().channelCount, 6);
    QCOMPARE(manager->device(QStringLiteral("devB"))->config().channelCount, 32);

    int channels = 0;
    manager->device(QStringLiteral("devB"))->ring()->consume(
        [&](const core::ChunkView& view) { channels = view.channelCount; });
    QCOMPARE(channels, 32);
}

// EQ-06 多链路：同一设备两条链路可独立连接/断开，命令按 linkId 路由。
void TstDeviceService::managerSupportsTwoLinks()
{
    FakeDeviceServer mainServer(6);
    FakeDeviceServer auxServer(6);

    auto manager = std::make_unique<service::DeviceManager>();

    service::LinkSpec mainLink;
    mainLink.id = QStringLiteral("main");
    mainLink.role = QStringLiteral("main");
    mainLink.protocol = QStringLiteral("custom");
    mainLink.host = QStringLiteral("127.0.0.1");
    mainLink.port = mainServer.port();

    service::LinkSpec auxLink;
    auxLink.id = QStringLiteral("aux");
    auxLink.role = QStringLiteral("aux");
    auxLink.protocol = QStringLiteral("custom");
    auxLink.host = QStringLiteral("127.0.0.1");
    auxLink.port = auxServer.port();

    service::DeviceSpec spec;
    spec.id = QStringLiteral("devL");
    spec.name = QStringLiteral("双链路设备");
    spec.host = QStringLiteral("127.0.0.1");
    spec.port = mainServer.port();
    spec.links = {mainLink, auxLink};

    QVERIFY(manager->addDevice(spec, core::DeviceConfig::make(6)));
    QCOMPARE(manager->linkIds(QStringLiteral("devL")),
             QStringList({QStringLiteral("main"), QStringLiteral("aux")}));
    QCOMPARE(manager->mainLinkId(QStringLiteral("devL")), QStringLiteral("main"));
    QVERIFY(manager->device(QStringLiteral("devL"))
            == manager->link(QStringLiteral("devL"), QStringLiteral("main")));

    // 只连主链路：辅助链路保持未连接。
    manager->connectLink(QStringLiteral("devL"), QStringLiteral("main"), mainLink);
    QVERIFY(waitUntil([&] {
        return manager->link(QStringLiteral("devL"), QStringLiteral("main"))->isConnected();
    }));
    QVERIFY(!manager->link(QStringLiteral("devL"), QStringLiteral("aux"))->isConnected());

    // 再连辅助链路：两条链路同时在线。
    manager->connectLink(QStringLiteral("devL"), QStringLiteral("aux"), auxLink);
    QVERIFY(waitUntil([&] {
        return manager->link(QStringLiteral("devL"), QStringLiteral("aux"))->isConnected();
    }));
    QVERIFY(manager->link(QStringLiteral("devL"), QStringLiteral("main"))->isConnected());

    // 测试引擎通过 linkId 路由：显式指定 aux，空则主链路。
    // 注意：sendCommand 会等待应答，必须在工作线程调用，主线程保持事件循环。
    test::ITestDevice* router = manager->testDevice(QStringLiteral("devL"));
    QVERIFY(router != nullptr);

    auto sendVia = [&](const QString& linkId) {
        std::atomic<bool> done{false};
        bool ok = false;
        quint8 status = 0xFF;
        std::thread worker([&] {
            ok = router->sendCommand(test::DeviceCommand::SetVout, 0, 6000, 1000,
                                     status, linkId);
            done.store(true);
        });
        const bool finished = waitUntil([&] { return done.load(); });
        worker.join();
        return std::make_tuple(finished && ok, status);
    };

    const auto auxResult = sendVia(QStringLiteral("aux"));
    QVERIFY(std::get<0>(auxResult));
    QCOMPARE(std::get<1>(auxResult), quint8(0));

    const auto mainResult = sendVia(QString());
    QVERIFY(std::get<0>(mainResult));
    QCOMPARE(std::get<1>(mainResult), quint8(0));

    // 断开辅助链路，主链路不受影响。
    manager->disconnectLink(QStringLiteral("devL"), QStringLiteral("aux"));
    QVERIFY(waitUntil([&] {
        return !manager->link(QStringLiteral("devL"), QStringLiteral("aux"))->isConnected();
    }));
    QVERIFY(manager->link(QStringLiteral("devL"), QStringLiteral("main"))->isConnected());
}

// EQ-06 收尾：链路健康快照 + 采样按链路路由（主链路断开后仍可从辅助链路采样）。
void TstDeviceService::managerLinkHealthAndSamplingRoute()
{
    FakeDeviceServer mainServer(6);
    FakeDeviceServer auxServer(6);

    auto manager = std::make_unique<service::DeviceManager>();

    service::LinkSpec mainLink;
    mainLink.id = QStringLiteral("main");
    mainLink.role = QStringLiteral("main");
    mainLink.protocol = QStringLiteral("custom");
    mainLink.host = QStringLiteral("127.0.0.1");
    mainLink.port = mainServer.port();

    service::LinkSpec auxLink;
    auxLink.id = QStringLiteral("aux");
    auxLink.role = QStringLiteral("aux");
    auxLink.protocol = QStringLiteral("custom");
    auxLink.host = QStringLiteral("127.0.0.1");
    auxLink.port = auxServer.port();

    service::DeviceSpec spec;
    spec.id = QStringLiteral("devH");
    spec.name = QStringLiteral("双链路健康");
    spec.links = {mainLink, auxLink};
    QVERIFY(manager->addDevice(spec, core::DeviceConfig::make(6)));

    manager->connectLink(QStringLiteral("devH"), QStringLiteral("main"), mainLink);
    manager->connectLink(QStringLiteral("devH"), QStringLiteral("aux"), auxLink);
    QVERIFY(waitUntil([&] {
        return manager->link(QStringLiteral("devH"), QStringLiteral("main"))->isConnected()
            && manager->link(QStringLiteral("devH"), QStringLiteral("aux"))->isConnected();
    }));

    // 等两条链路都有入站数据，健康快照才有意义。
    QVERIFY(waitUntil([&] {
        const QVector<service::LinkHealth> health = manager->linkHealth(QStringLiteral("devH"));
        if (health.size() != 2)
            return false;
        for (const service::LinkHealth& item : health) {
            if (!item.connected || item.frames == 0 || item.lastInboundAgeMs < 0)
                return false;
        }
        return true;
    }));

    const QVector<service::LinkHealth> health = manager->linkHealth(QStringLiteral("devH"));
    QCOMPARE(health.size(), 2);
    QStringList linkIds;
    for (const service::LinkHealth& item : health) {
        linkIds.append(item.linkId);
        QCOMPARE(item.deviceId, QStringLiteral("devH"));
        QCOMPARE(item.protocol, QStringLiteral("custom"));
        QVERIFY(item.connected);
        QVERIFY(item.bytes > 0);
        QVERIFY(item.lastInboundAgeMs >= 0);
    }
    QCOMPARE(linkIds, QStringList({QStringLiteral("main"), QStringLiteral("aux")}));

    // 断开主链路：辅助链路仍在采集，按 linkId 路由的单点/窗口采样应成功。
    manager->disconnectLink(QStringLiteral("devH"), QStringLiteral("main"));
    QVERIFY(waitUntil([&] {
        return !manager->link(QStringLiteral("devH"), QStringLiteral("main"))->isConnected();
    }));

    test::ITestDevice* router = manager->testDevice(QStringLiteral("devH"));
    QVERIFY(router != nullptr);

    auto sampleVia = [&](const QString& linkId) {
        std::atomic<bool> done{false};
        std::vector<std::int16_t> samples;
        std::thread worker([&] {
            samples = router->sampleWindow(0, 100, linkId);
            done.store(true);
        });
        waitUntil([&] { return done.load(); });
        worker.join();
        return samples;
    };

    QVERIFY(!sampleVia(QStringLiteral("aux")).empty());
    QVERIFY(sampleVia(QString()).empty()); // 主链路已断开

    // 单点测量同样按链路路由。
    std::atomic<bool> done{false};
    double value = 0.0;
    QString error;
    bool ok = false;
    std::thread worker([&] {
        ok = router->measureOnce(0, 200, &value, &error, QStringLiteral("aux"));
        done.store(true);
    });
    waitUntil([&] { return done.load(); });
    worker.join();
    QVERIFY2(ok, qPrintable(error));
}

// fleet.json：links 数组生效；旧单 protocol 格式解析为唯一主链路。
void TstDeviceService::loadsFleetWithLinksAndLegacy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("fleet.json"));

    auto writeFleet = [&path](const QByteArray& json) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        return file.write(json) == json.size();
    };

    // 旧格式：单 protocol 字段。
    QVERIFY(writeFleet(R"({"devices":[{"id":"old","name":"旧设备","protocol":"custom",)"
                       R"("host":"127.0.0.1","port":19527}]})"));
    auto manager = std::make_unique<service::DeviceManager>();
    QVERIFY(manager->loadFleet(path));
    QCOMPARE(manager->linkIds(QStringLiteral("old")), QStringList({QStringLiteral("main")}));
    QCOMPARE(manager->spec(QStringLiteral("old")).name, QStringLiteral("旧设备"));

    // 新格式：links 数组，主/辅两条链路。
    QVERIFY(writeFleet(R"({"devices":[{"id":"multi","name":"多链路","links":[)"
                       R"({"id":"main","role":"main","protocol":"custom","host":"127.0.0.1","port":19528},)"
                       R"({"id":"aux","role":"aux","protocol":"scpi","host":"127.0.0.1","port":19529}]}]})"));
    QVERIFY(manager->loadFleet(path));
    QCOMPARE(manager->linkIds(QStringLiteral("multi")),
             QStringList({QStringLiteral("main"), QStringLiteral("aux")}));
    QCOMPARE(manager->mainLinkId(QStringLiteral("multi")), QStringLiteral("main"));
    // 设备级字段同步为主链路，界面按旧方式展示不回归。
    QCOMPARE(manager->spec(QStringLiteral("multi")).protocol, QStringLiteral("custom"));
    QCOMPARE(manager->linkSpec(QStringLiteral("multi"), QStringLiteral("aux")).protocol,
             QStringLiteral("scpi"));
}

// 每秒摘要：收到的数据应聚合成 6 通道的 min/max/avg/count。
void TstDeviceService::emitsSecondSummaries()
{
    qRegisterMetaType<service::DeviceSummary>();

    FakeDeviceServer server;
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));
    QSignalSpy summaries(device.get(), &service::DeviceService::summaryReady);

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));
    QVERIFY2(waitUntil([&] { return summaries.count() >= 1; }, 4000),
             "4 秒内没有产生秒级摘要");

    const auto summary = summaries.first().at(0).value<service::DeviceSummary>();
    QCOMPARE(summary.channels.size(), 6);
    QVERIFY(summary.epochSec > 0);

    const auto& channel0 = summary.channels.at(0);
    QVERIFY(channel0.sampleCount > 0);
    QVERIFY(qAbs(channel0.avgValue - 5000.0) < 0.01);
    QCOMPARE(channel0.minValue, 5000.0);
    QCOMPARE(channel0.maxValue, 5000.0);
}

// 端到端：引擎（工作线程）→ 设备服务 → 假设备 → 判定通过。
void TstDeviceService::engineRunsFlowEndToEnd()
{
    FakeDeviceServer server;
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));
    QTest::qWait(50);

    test::TestFlow flow;
    flow.schemaVersion = 1;
    flow.id = QStringLiteral("integration");
    flow.name = QStringLiteral("integration");
    flow.defaultTimeoutMs = 500;
    flow.onFailure = QStringLiteral("abort");

    test::TestStep command;
    command.id = QStringLiteral("cmd");
    command.name = QStringLiteral("set vout");
    command.action = test::StepAction::Command;
    command.cmd = QStringLiteral("SET_VOUT");
    command.valueMv = 5000;
    command.expect.present = true;
    command.expect.ackStatus = 0;

    test::TestStep delay;
    delay.id = QStringLiteral("wait");
    delay.name = QStringLiteral("wait");
    delay.action = test::StepAction::Delay;
    delay.delayMs = 30;

    test::TestStep measure;
    measure.id = QStringLiteral("measure");
    measure.name = QStringLiteral("measure");
    measure.action = test::StepAction::Measure;
    measure.channel = 0;
    measure.windowMs = 100;
    measure.expect.present = true;
    measure.expect.metric = test::Metric::Avg;
    measure.expect.hasMin = true;
    measure.expect.min = 4750;
    measure.expect.hasMax = true;
    measure.expect.max = 5250;

    flow.steps = {command, delay, measure};

    test::RealClock clock;
    test::TestEngine engine(device.get(), &clock);

    std::atomic<bool> done{false};
    test::RunResult result;

    std::thread worker([&] {
        result = engine.run(flow);
        done.store(true);
    });

    QVERIFY(waitUntil([&] { return done.load(); }, 8000));
    worker.join();

    QCOMPARE(static_cast<int>(result.verdict),
             static_cast<int>(test::RunVerdict::Pass));
    QCOMPARE(result.steps.size(), 3);
    QVERIFY(result.steps.at(2).hasMeasured);
    QVERIFY(qAbs(result.steps.at(2).measured - 5000.0) < 1.0);
}

// Modbus TCP：轮询读寄存器 → 环形缓冲 → 采样窗口。
void TstDeviceService::modbusPollerProducesSamples()
{
    FakeModbusServer server;
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));

    device->connectModbusTcp(QStringLiteral("127.0.0.1"), server.port(), 1);
    QVERIFY(waitUntil([&] { return device->isConnected(); }));

    // 20ms 轮询一次，累计到 5 个采样块即可认为链路正常。
    QVERIFY(waitUntil([&] { return device->stats().blocks >= 5; }, 5000));
    QCOMPARE(device->sampleRateHz(), 50);

    std::atomic<bool> done{false};
    std::vector<std::int16_t> samples;
    std::thread worker([&] {
        samples = device->sampleWindow(0, 100);
        done.store(true);
    });
    QVERIFY(waitUntil([&] { return done.load(); }, 3000));
    worker.join();

    QVERIFY(!samples.empty());
    QCOMPARE(static_cast<int>(samples.front()), 5000);
}

// Modbus 写命令：测试流程的 SET_VOUT 应转成 FC06 并拿到成功应答。
void TstDeviceService::modbusWriteCommandUpdatesRegister()
{
    FakeModbusServer server;
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));

    device->connectModbusTcp(QStringLiteral("127.0.0.1"), server.port(), 1);
    QVERIFY(waitUntil([&] { return device->isConnected(); }));

    std::atomic<bool> done{false};
    bool ok = false;
    quint8 status = 0xFF;

    std::thread worker([&] {
        ok = device->sendCommand(test::DeviceCommand::SetVout, 0, 6000, 1000, status);
        done.store(true);
    });

    QVERIFY(waitUntil([&] { return done.load(); }, 5000));
    worker.join();

    QVERIFY(ok);
    QCOMPARE(static_cast<int>(status), 0);
    QCOMPARE(static_cast<int>(server.lastWrittenValue()), 6000);
}

// Modbus 版本读取：FC03 读 0x0100 起 12 个保持寄存器 → ASCII 版本串。
void TstDeviceService::queryTextReadsModbusVersion()
{
    FakeModbusServer server;
    auto device = std::make_unique<service::DeviceService>(nullptr, core::DeviceConfig::make(6));

    device->connectModbusTcp(QStringLiteral("127.0.0.1"), server.port(), 1);
    QVERIFY(waitUntil([&] { return device->isConnected(); }));

    std::atomic<bool> done{false};
    bool ok = false;
    QString value;
    QString error;

    std::thread worker([&] {
        ok = device->queryText(QString(), 1000, &value, &error);
        done.store(true);
    });

    QVERIFY(waitUntil([&] { return done.load(); }, 5000));
    worker.join();

    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(value, QStringLiteral("AegisSim,DMM,0001,1.0"));
}

// 阈值报警：越限 → 回差内保持 → 恢复 → 另一方向越限，报警记录与计数同步。
void TstDeviceService::alarmMonitorRaisesAndClears()
{
    QTemporaryDir dir;
    storage::SystemRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    service::DeviceManager manager;
    service::DeviceSpec spec;
    spec.id = QStringLiteral("dev1");
    spec.name = QStringLiteral("测试设备");
    spec.autoConnect = false;

    core::DeviceConfig config = core::DeviceConfig::make(2);
    config.channels[0].hasHigh = true;
    config.channels[0].high = 6000.0;
    config.channels[0].deadband = 50.0;
    config.channels[1].hasLow = true;
    config.channels[1].low = 100.0;
    config.channels[1].deadband = 10.0;
    QVERIFY(manager.addDevice(spec, config));

    service::AlarmMonitor monitor(&manager, &repository);
    int raised = 0;
    int cleared = 0;
    connect(&monitor, &service::AlarmMonitor::alarmRaised, this,
            [&](const storage::AlarmRecord&) { ++raised; });
    connect(&monitor, &service::AlarmMonitor::alarmCleared, this,
            [&](int, const QString&) { ++cleared; });

    auto summaryWith = [](double ch0, double ch1) {
        service::DeviceSummary summary;
        summary.epochSec = 1;
        summary.channels.resize(2);
        summary.channels[0].avgValue = ch0;
        summary.channels[0].sampleCount = 100;
        summary.channels[1].avgValue = ch1;
        summary.channels[1].sampleCount = 100;
        return summary;
    };

    // 正常范围：不报警。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(5000.0, 500.0));
    QCOMPARE(raised, 0);
    QCOMPARE(repository.activeAlarmCount(), 0);

    // 高于上限：进入高报警并落库。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(6500.0, 500.0));
    QCOMPARE(raised, 1);
    QCOMPARE(monitor.openCount(), 1);
    QCOMPARE(repository.activeAlarmCount(), 1);

    // 回差内（6000-50 以上）：仍处于报警，不重复记录。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(5980.0, 500.0));
    QCOMPARE(raised, 1);
    QCOMPARE(cleared, 0);

    // 回落超过回差：恢复。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(5900.0, 500.0));
    QCOMPARE(cleared, 1);
    QCOMPARE(monitor.openCount(), 0);
    QCOMPARE(repository.activeAlarmCount(), 0);

    // 低于下限：进入低报警。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(5000.0, 50.0));
    QCOMPARE(raised, 2);
    QCOMPARE(repository.activeAlarmCount(), 1);

    const auto alarms = repository.recentAlarms(10, false);
    QCOMPARE(alarms.size(), 2);
    QVERIFY(alarms.first().level == QLatin1String("LOW"));
}

// ISA-18.2 搁置：搁置期间不产生新报警，已存在的报警仍按条件恢复。
void TstDeviceService::alarmShelveSuppressesNewAlarms()
{
    QTemporaryDir dir;
    storage::SystemRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    service::DeviceManager manager;
    service::DeviceSpec spec;
    spec.id = QStringLiteral("dev1");
    spec.name = QStringLiteral("测试设备");
    spec.autoConnect = false;

    core::DeviceConfig config = core::DeviceConfig::make(1);
    config.channels[0].hasHigh = true;
    config.channels[0].high = 6000.0;
    config.channels[0].deadband = 50.0;
    QVERIFY(manager.addDevice(spec, config));

    service::AlarmMonitor monitor(&manager, &repository);
    int raised = 0;
    int cleared = 0;
    connect(&monitor, &service::AlarmMonitor::alarmRaised, this,
            [&](const storage::AlarmRecord&) { ++raised; });
    connect(&monitor, &service::AlarmMonitor::alarmCleared, this,
            [&](int, const QString&) { ++cleared; });

    auto summaryWith = [](double ch0) {
        service::DeviceSummary summary;
        summary.epochSec = 1;
        summary.channels.resize(1);
        summary.channels[0].avgValue = ch0;
        summary.channels[0].sampleCount = 100;
        return summary;
    };

    // 先产生一条高报警，再搁置该通道。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(6500.0));
    QCOMPARE(raised, 1);
    monitor.shelve(QStringLiteral("dev1"), 0, 10);
    QVERIFY(monitor.isShelved(QStringLiteral("dev1"), 0));

    // 已存在的报警仍可恢复。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(5900.0));
    QCOMPARE(cleared, 1);
    QCOMPARE(monitor.openCount(), 0);

    // 搁置期内再次越限：不再产生新报警、不落库。
    monitor.onSummary(QStringLiteral("dev1"), summaryWith(6800.0));
    QCOMPARE(raised, 1);
    QCOMPARE(monitor.openCount(), 0);
    QCOMPARE(repository.activeAlarmCount(), 0);
}

// 设备配置：校准参数与日常点检金样值解析。
void TstDeviceService::deviceConfigParsesCalibrationAndCheck()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("devices.json"));

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"JSON({
      "channelCount": 2,
      "sampleRateHz": 1000,
      "channels": [
        { "name": "CH0", "unit": "mV", "gain": 1.05, "offset": -12,
          "check": 5000, "checkTolerance": 20 },
        { "name": "CH1", "unit": "mA" }
      ]
    })JSON");
    file.close();

    QString error;
    const core::DeviceConfig config = core::DeviceConfig::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(config.channelCount, 2);
    QVERIFY(config.hasCalibration());

    const core::DeviceChannel& first = config.channels.at(0);
    QCOMPARE(first.gain, 1.05);
    QCOMPARE(first.offset, -12.0);
    QVERIFY(first.hasCheck);
    QCOMPARE(first.checkValue, 5000.0);
    QCOMPARE(first.checkTolerance, 20.0);

    const core::DeviceChannel& second = config.channels.at(1);
    QCOMPARE(second.gain, 1.0);
    QCOMPARE(second.offset, 0.0);
    QVERIFY(!second.hasCheck);

    // 默认配置没有校准，避免无谓的逐点运算。
    QVERIFY(!core::DeviceConfig::make(4).hasCalibration());
}

// 断线自动重连：服务端踢掉客户端后，1 秒退避内应重新连上；
// 手动断开后则不再重连。
void TstDeviceService::autoReconnectsAfterDrop()
{
    FakeDeviceServer server;
    auto device = std::make_unique<service::DeviceService>(
        nullptr, core::DeviceConfig::make(6));

    QStringList logs;
    QObject::connect(device.get(), &service::DeviceService::logMessage,
                     device.get(), [&logs](const QString& message) {
                         logs.append(message);
                     }, Qt::DirectConnection);

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));

    // 服务端主动断开：应先变为未连接，再自动连回。
    server.dropClients();
    QVERIFY(waitUntil([&] { return !device->isConnected(); }, 5000));
    QVERIFY(waitUntil([&] { return device->isConnected(); }, 8000));
    QVERIFY(logs.filter(QStringLiteral("自动重连")).size() >= 1);

    // 手动断开后，服务端再踢一次也不应重连。
    device->disconnectDevice();
    QVERIFY(waitUntil([&] { return !device->isConnected(); }, 5000));
    server.dropClients();
    QTest::qWait(1500);
    QVERIFY(!device->isConnected());
}

// 心跳（EQ-03）：自定义协议主动发 Ping；长时间无入站数据判定超时并断开重连。
void TstDeviceService::heartbeatDetectsSilentLink()
{
    FakeSilentServer server;
    auto device = std::make_unique<service::DeviceService>(
        nullptr, core::DeviceConfig::make(6));
    // 缩短周期/超时，避免测试等待十余秒。
    device->setHeartbeatTiming(50, 200);

    QStringList logs;
    QObject::connect(device.get(), &service::DeviceService::logMessage,
                     device.get(), [&logs](const QString& message) {
                         logs.append(message);
                     }, Qt::DirectConnection);

    device->connectTcp(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(waitUntil([&] { return device->isConnected(); }));

    // 心跳发送：服务端应收到 Ping 帧。
    QVERIFY2(waitUntil([&] { return server.pingCount() > 0; }, 3000),
             "未收到心跳 Ping 帧");

    // 没有任何入站数据 → 判定心跳超时并断开（随后自动重连不影响本断言）。
    QVERIFY(waitUntil([&] { return !device->isConnected(); }, 5000));
    QVERIFY(logs.filter(QStringLiteral("心跳超时")).size() >= 1);
}

QTEST_GUILESS_MAIN(TstDeviceService)
#include "tst_deviceservice.moc"
