// =============================================================================
// opcua_probe.cpp —— OPC UA 快速验证工具
//
// 用途：不启动 GUI，也能验证"订阅是否生效、写节点是否成功"。
//
// 用法：
//   aegis_opcua_probe opc.tcp://127.0.0.1:4840
//       订阅 ns=1;s=Channel0..5，打印一次快照后退出。
//
//   aegis_opcua_probe opc.tcp://127.0.0.1:4840 0 7000
//       订阅后写节点 0（输出电压，单位 mV），再打印一次快照确认写入生效。
//
// 退出码：0 成功，1 参数错误，2 连接失败，3 超时。
// =============================================================================

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

#include "infra/opcua_client.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    if (args.size() < 2) {
        std::printf("用法: %s <endpoint> [index value]\n"
                    "  例: %s opc.tcp://127.0.0.1:4840\n"
                    "  例: %s opc.tcp://127.0.0.1:4840 0 7000\n",
                    qUtf8Printable(args.value(0)), qUtf8Printable(args.value(0)),
                    qUtf8Printable(args.value(0)));
        return 1;
    }

    const QString endpoint = args.at(1);
    const bool doWrite = args.size() >= 4;
    const int writeIndex = doWrite ? args.at(2).toInt() : -1;
    const double writeValue = doWrite ? args.at(3).toDouble() : 0.0;

    aegis::infra::OpcUaClient client;
    int updates = 0;
    int exitCode = 0;
    // 只读模式：收到第一份快照即可退出。
    // 写入模式：要等到"写完成之后"的一份快照，才能确认写入已生效。
    bool canQuitOnSnapshot = !doWrite;

    QObject::connect(&client, &aegis::infra::OpcUaClient::failed,
                     [](const QString& reason) {
                         std::fprintf(stderr, "[error] %s\n", qUtf8Printable(reason));
                     });

    QObject::connect(&client, &aegis::infra::OpcUaClient::opened, [&] {
        std::printf("[ok] 已连接 %s，订阅已建立\n", qUtf8Printable(endpoint));
        if (doWrite) {
            std::printf("[info] 写节点 %d = %.1f\n", writeIndex, writeValue);
            client.writeValue(1, writeIndex, writeValue);
        }
    });

    QObject::connect(&client, &aegis::infra::OpcUaClient::writeCompleted,
                     [&](quint16, bool ok, quint8 status) {
                         std::printf("[%s] 写节点结果 status=%u\n", ok ? "ok" : "fail",
                                     static_cast<unsigned>(status));
                         if (!ok) {
                             exitCode = 2;
                             app.quit();
                             return;
                         }
                         // 写已经完成：允许下一份快照（无论先后）触发退出。
                         canQuitOnSnapshot = true;
                         if (updates > 0)
                             app.quit();
                     });

    QObject::connect(&client, &aegis::infra::OpcUaClient::valuesUpdated,
                     [&](const QVector<double>& values) {
                         ++updates;
                         std::printf("[values#%d]", updates);
                         for (int i = 0; i < values.size(); ++i)
                             std::printf(" ch%d=%.1f", i, values.at(i));
                         std::printf("\n");

                         if (canQuitOnSnapshot)
                             app.quit();
                     });

    // 连接失败时 failed 只会打印日志，这里靠超时兜底退出。
    QTimer::singleShot(8000, &app, [&] {
        if (updates == 0) {
            std::fprintf(stderr, "[error] 8 秒内没有收到任何订阅数据\n");
            exitCode = 3;
        }
        app.quit();
    });

    client.open(endpoint, {QStringLiteral("ns=1;s=Channel0"), QStringLiteral("ns=1;s=Channel1"),
                           QStringLiteral("ns=1;s=Channel2"), QStringLiteral("ns=1;s=Channel3"),
                           QStringLiteral("ns=1;s=Channel4"), QStringLiteral("ns=1;s=Channel5")});

    app.exec();
    client.close();
    return exitCode;
}
