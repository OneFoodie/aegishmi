// =============================================================================
// tst_opcua —— OPC UA 客户端集成测试
//
// 测试思路：
//   1. 在本进程内启动一个最小 OPC UA 服务端（独立线程运行事件循环），
//      命名空间 1 下暴露 Channel0/Channel1 两个 double 变量；
//   2. 用工程里的 OpcUaClient 连接该服务端并轮询；
//   3. 校验收到 valuesUpdated 信号且数值与写入一致。
//
// 为什么不连模拟器进程：单元测试应当自包含、可重复，不依赖外部进程和固定端口占用。
// 未启用 open62541 的构建会 QSKIP，不影响其余测试。
// =============================================================================

#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QVector>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "infra/opcua_client.h"
#include "service/device_service.h"

#ifdef AEGIS_HAS_OPCUA
#include <open62541.h>
#endif

namespace {

#ifdef AEGIS_HAS_OPCUA

// 测试服务端：一个后台线程持续驱动 UA_Server_run_iterate。
// 必须放在独立线程，因为 UA_Client_connect 是同步阻塞的：
// 如果服务端和客户端在同一线程，connect 期间服务端无法处理请求，会一直等到超时。
class TestUaServer {
public:
    TestUaServer() = default;

    ~TestUaServer()
    {
        stop();
    }

    // 在指定端口启动服务端，并写入两个初始值。
    bool start(quint16 port, double channel0, double channel1)
    {
        server_ = UA_Server_new();
        if (server_ == nullptr)
            return false;

        // setMinimal 会创建监听 socket，并使用 SecurityPolicyNone + 匿名登录（仅测试用）。
        UA_StatusCode status =
            UA_ServerConfig_setMinimal(UA_Server_getConfig(server_), port, nullptr);
        if (status != UA_STATUSCODE_GOOD) {
            UA_Server_delete(server_);
            server_ = nullptr;
            return false;
        }

        const double values[2] = {channel0, channel1};
        static const char* names[2] = {"Channel0", "Channel1"};
        for (int i = 0; i < 2; ++i) {
            UA_VariableAttributes attributes = UA_VariableAttributes_default;
            UA_Variant_setScalar(&attributes.value, const_cast<double*>(&values[i]),
                                 &UA_TYPES[UA_TYPES_DOUBLE]);
            attributes.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
            // 节点 0 可写，用于验证客户端写节点；节点 1 只读。
            attributes.accessLevel = (i == 0)
                ? (UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE)
                : UA_ACCESSLEVELMASK_READ;
            attributes.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"),
                                                      const_cast<char*>(names[i]));

            UA_Server_addVariableNode(server_, UA_NODEID_STRING(1, const_cast<char*>(names[i])),
                                      UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                      UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                      UA_QUALIFIEDNAME(1, const_cast<char*>(names[i])),
                                      UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                                      attributes, nullptr, nullptr);
        }

        // 固件版本节点（String，只读），用于验证 queryText 的版本读取。
        {
            UA_String version = UA_STRING_ALLOC("AegisSim,DMM,0001,1.0");
            UA_VariableAttributes attributes = UA_VariableAttributes_default;
            UA_Variant_setScalar(&attributes.value, &version, &UA_TYPES[UA_TYPES_STRING]);
            attributes.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
            attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
            attributes.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"),
                                                      const_cast<char*>("FirmwareVersion"));
            UA_Server_addVariableNode(
                server_, UA_NODEID_STRING(1, const_cast<char*>("FirmwareVersion")),
                UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                UA_QUALIFIEDNAME(1, const_cast<char*>("FirmwareVersion")),
                UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                attributes, nullptr, nullptr);
            UA_String_clear(&version);
        }

        if (UA_Server_run_startup(server_) != UA_STATUSCODE_GOOD) {
            UA_Server_delete(server_);
            server_ = nullptr;
            return false;
        }

        running_.store(true);
        thread_ = std::thread([this] {
            while (running_.load(std::memory_order_relaxed)) {
                UA_Server_run_iterate(server_, false);
                // 5ms 一轮：既保证请求能被及时处理，又不空转吃满 CPU。
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        return true;
    }

    void stop()
    {
        if (server_ == nullptr)
            return;

        running_.store(false);
        if (thread_.joinable())
            thread_.join();

        UA_Server_run_shutdown(server_);
        UA_Server_delete(server_);
        server_ = nullptr;
    }

private:
    UA_Server* server_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

#endif // AEGIS_HAS_OPCUA

} // namespace

class TestOpcUa : public QObject {
    Q_OBJECT

private slots:
    // 核心用例：客户端能连上服务端、按 100ms 周期读回节点值。
    void readsNodeValues()
    {
#ifndef AEGIS_HAS_OPCUA
        QSKIP("本次构建未启用 OPC UA（缺少 third_party/open62541）");
#else
        TestUaServer server;
        QVERIFY2(server.start(4851, 1234.0, 5678.0), "测试用 OPC UA 服务端启动失败");

        aegis::infra::OpcUaClient client;
        QSignalSpy opened(&client, &aegis::infra::OpcUaClient::opened);
        QSignalSpy failed(&client, &aegis::infra::OpcUaClient::failed);
        QSignalSpy updated(&client, &aegis::infra::OpcUaClient::valuesUpdated);

        client.open(QStringLiteral("opc.tcp://127.0.0.1:4851"),
                    {QStringLiteral("ns=1;s=Channel0"), QStringLiteral("ns=1;s=Channel1")});

        // open 是同步的：返回后信号已经发出，可以直接断言。
        QVERIFY2(opened.count() == 1,
                 qPrintable(failed.isEmpty()
                                ? QStringLiteral("未收到 opened 信号")
                                : failed.first().at(0).toString()));

        // 等待至少一次轮询结果（周期 100ms，给 3 秒余量）。
        QVERIFY2(updated.wait(3000), "3 秒内没有收到 valuesUpdated");
        const QVector<double> values = updated.first().at(0).value<QVector<double>>();
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.at(0), 1234.0);
        QCOMPARE(values.at(1), 5678.0);

        client.close();
        server.stop();
#endif
    }

    // 写节点：客户端写入后，另一个客户端从服务端读回的值应同步变化。
    void writesNodeValue()
    {
#ifndef AEGIS_HAS_OPCUA
        QSKIP("本次构建未启用 OPC UA（缺少 third_party/open62541）");
#else
        TestUaServer server;
        QVERIFY2(server.start(4852, 1000.0, 2000.0), "测试用 OPC UA 服务端启动失败");

        aegis::infra::OpcUaClient writer;
        QSignalSpy opened(&writer, &aegis::infra::OpcUaClient::opened);
        QSignalSpy failed(&writer, &aegis::infra::OpcUaClient::failed);
        QSignalSpy written(&writer, &aegis::infra::OpcUaClient::writeCompleted);

        writer.open(QStringLiteral("opc.tcp://127.0.0.1:4852"),
                    {QStringLiteral("ns=1;s=Channel0"), QStringLiteral("ns=1;s=Channel1")});
        QVERIFY2(opened.count() == 1,
                 qPrintable(failed.isEmpty()
                                ? QStringLiteral("未收到 opened 信号")
                                : failed.first().at(0).toString()));

        // 写节点 0（对应输出设定值），事务号 7。
        writer.writeValue(7, 0, 4321.0);
        QCOMPARE(written.count(), 1);
        QCOMPARE(written.first().at(0).value<quint16>(), static_cast<quint16>(7));
        QCOMPARE(written.first().at(1).toBool(), true);

        // 用第二个客户端订阅同一节点，确认服务端真的保存了写入值。
        aegis::infra::OpcUaClient reader;
        QSignalSpy readerOpened(&reader, &aegis::infra::OpcUaClient::opened);
        QSignalSpy readerFailed(&reader, &aegis::infra::OpcUaClient::failed);
        QSignalSpy readerUpdated(&reader, &aegis::infra::OpcUaClient::valuesUpdated);

        reader.open(QStringLiteral("opc.tcp://127.0.0.1:4852"),
                    {QStringLiteral("ns=1;s=Channel0")});
        QVERIFY2(readerOpened.count() == 1,
                 qPrintable(readerFailed.isEmpty()
                                ? QStringLiteral("读取客户端未连接")
                                : readerFailed.first().at(0).toString()));
        QVERIFY2(readerUpdated.wait(3000), "3 秒内没有收到读取客户端的订阅通知");
        QCOMPARE(readerUpdated.first().at(0).value<QVector<double>>().at(0), 4321.0);

        reader.close();
        writer.close();
        server.stop();
#endif
    }

    // DeviceService 链路：connectOpcUa → sendCommand(SET_VOUT) → 服务端节点值变化。
    // sendCommand 模拟测试引擎在独立线程阻塞等待，主线程继续处理队列信号。
    void deviceServiceWritesSetpoint()
    {
#ifndef AEGIS_HAS_OPCUA
        QSKIP("本次构建未启用 OPC UA（缺少 third_party/open62541）");
#else
        TestUaServer server;
        QVERIFY2(server.start(4853, 1000.0, 2000.0), "测试用 OPC UA 服务端启动失败");

        // DeviceService 内含约 2MB 环形缓冲，必须放堆上（Windows 线程栈默认 1MB）。
        auto device = std::make_unique<aegis::service::DeviceService>();

        QSignalSpy connected(device.get(), &aegis::service::DeviceService::connectedChanged);
        QStringList logs;
        connect(device.get(), &aegis::service::DeviceService::logMessage, this,
                [&logs](const QString& message) { logs.append(message); });

        device->connectOpcUa(QStringLiteral("opc.tcp://127.0.0.1:4853"));
        QVERIFY2(connected.wait(5000),
                 qPrintable(QStringLiteral("5 秒内没有收到连接结果；日志：") + logs.join(" | ")));
        QVERIFY2(connected.first().at(0).toBool(),
                 qPrintable(connected.first().at(1).toString()));

        std::atomic<bool> ok{false};
        std::atomic<int> status{0xFF};
        std::thread worker([&] {
            quint8 result = 0xFF;
            const bool sent = device->sendCommand(aegis::test::DeviceCommand::SetVout, 0, 7000, 2000,
                                                  result);
            status.store(result);
            ok.store(sent);
        });

        // QTRY 会持续处理事件循环：writeCompleted 的队列连接才能执行并唤醒 worker。
        QTRY_VERIFY_WITH_TIMEOUT(ok.load(), 4000);
        worker.join();
        QVERIFY(ok.load());
        QCOMPARE(status.load(), 0);

        // 用独立客户端确认服务端保存了新设定值。
        aegis::infra::OpcUaClient reader;
        QSignalSpy readerOpened(&reader, &aegis::infra::OpcUaClient::opened);
        QSignalSpy readerUpdated(&reader, &aegis::infra::OpcUaClient::valuesUpdated);
        reader.open(QStringLiteral("opc.tcp://127.0.0.1:4853"),
                    {QStringLiteral("ns=1;s=Channel0")});
        QCOMPARE(readerOpened.count(), 1);
        QVERIFY2(readerUpdated.wait(3000), "未收到读取客户端的订阅通知");
        QCOMPARE(readerUpdated.first().at(0).value<QVector<double>>().at(0), 7000.0);

        reader.close();
        device->disconnectDevice();
        server.stop();
#endif
    }

    // 版本节点：ns=1;s=FirmwareVersion 应能读回 String；DeviceService 默认读该节点。
    void readsFirmwareVersionNode()
    {
#ifndef AEGIS_HAS_OPCUA
        QSKIP("本次构建未启用 OPC UA（缺少 third_party/open62541）");
#else
        TestUaServer server;
        QVERIFY2(server.start(4854, 1000.0, 2000.0), "测试用 OPC UA 服务端启动失败");

        aegis::infra::OpcUaClient client;
        QSignalSpy opened(&client, &aegis::infra::OpcUaClient::opened);
        QSignalSpy failed(&client, &aegis::infra::OpcUaClient::failed);
        QSignalSpy read(&client, &aegis::infra::OpcUaClient::readCompleted);

        client.open(QStringLiteral("opc.tcp://127.0.0.1:4854"),
                    {QStringLiteral("ns=1;s=Channel0"), QStringLiteral("ns=1;s=Channel1")});
        QVERIFY2(opened.count() == 1,
                 qPrintable(failed.isEmpty()
                                ? QStringLiteral("未收到 opened 信号")
                                : failed.first().at(0).toString()));

        // 读版本节点：同步返回，信号在调用期间已发出。
        client.readString(9, QStringLiteral("ns=1;s=FirmwareVersion"));
        QCOMPARE(read.count(), 1);
        QCOMPARE(read.first().at(0).value<quint16>(), static_cast<quint16>(9));
        QCOMPARE(read.first().at(1).toBool(), true);
        QCOMPARE(read.first().at(2).toString(), QStringLiteral("AegisSim,DMM,0001,1.0"));

        // 不存在的节点：读取失败而不是返回空串成功。
        client.readString(10, QStringLiteral("ns=1;s=NoSuchNode"));
        QCOMPARE(read.count(), 2);
        QCOMPARE(read.at(1).at(1).toBool(), false);

        // DeviceService 链路：queryText 默认读取 FirmwareVersion 节点。
        auto device = std::make_unique<aegis::service::DeviceService>();
        QSignalSpy connected(device.get(), &aegis::service::DeviceService::connectedChanged);
        device->connectOpcUa(QStringLiteral("opc.tcp://127.0.0.1:4854"));
        QVERIFY2(connected.wait(5000), "DeviceService 未连接");
        QVERIFY(connected.first().at(0).toBool());

        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
        QString value;
        std::thread worker([&] {
            ok.store(device->queryText(QString(), 2000, &value, nullptr));
            done.store(true);
        });
        QTRY_VERIFY_WITH_TIMEOUT(done.load(), 5000);
        worker.join();

        QVERIFY(ok.load());
        QCOMPARE(value, QStringLiteral("AegisSim,DMM,0001,1.0"));

        device->disconnectDevice();
        client.close();
        server.stop();
#endif
    }

    // 连接不存在的端点应当失败，而不是一直阻塞。
    void failsOnBadEndpoint()
    {
#ifndef AEGIS_HAS_OPCUA
        QSKIP("本次构建未启用 OPC UA（缺少 third_party/open62541）");
#else
        aegis::infra::OpcUaClient client;
        QSignalSpy failed(&client, &aegis::infra::OpcUaClient::failed);

        // 端口 4899 在本测试环境没有服务端；客户端应在默认超时后返回错误。
        client.open(QStringLiteral("opc.tcp://127.0.0.1:4899"),
                    {QStringLiteral("ns=1;s=Channel0")});

        QCOMPARE(failed.count(), 1);
#endif
    }
};

QTEST_MAIN(TestOpcUa)
#include "tst_opcua.moc"
