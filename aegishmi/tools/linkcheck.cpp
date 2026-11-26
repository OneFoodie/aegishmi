// =============================================================================
// linkcheck.cpp —— 无界面连接测试工具（多设备/多链路）
//
// 用途：按 fleet.json 加载设备清单，连接全部链路，等待入站数据后输出结果。
// 适合在模拟器集群（如 10 台模拟仪表）启动后做一次联通性验收。
//
// 运行：aegis_linkcheck.exe --fleet configs\devices\fleet_10sim.json [--timeout 15000]
// 退出码：0 = 全部链路在线且有数据；1 = 存在未连接/无数据链路；2 = 清单加载失败
// =============================================================================

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>

#include <cstdio>

#include "service/device_manager.h"
#include "service/device_service.h"

using namespace aegis;

namespace {

// 一边处理事件（接收连接状态/入站数据）一边等待，避免阻塞 Qt 事件循环。
void pumpEvents(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
}

bool allLinksConnected(service::DeviceManager& manager)
{
    const QStringList devices = manager.ids();
    if (devices.isEmpty())
        return false;

    for (const QString& id : devices) {
        for (const QString& linkId : manager.linkIds(id)) {
            service::DeviceService* service = manager.link(id, linkId);
            if (service == nullptr || !service->isConnected())
                return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QString fleetPath;
    int timeoutMs = 15000;
    int settleMs = 800;

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        const auto nextValue = [&args, &i] {
            return (i + 1 < args.size()) ? args.at(++i) : QString();
        };
        if (arg == QLatin1String("--fleet"))
            fleetPath = nextValue();
        else if (arg == QLatin1String("--timeout"))
            timeoutMs = nextValue().toInt();
        else if (arg == QLatin1String("--settle"))
            settleMs = nextValue().toInt();
    }

    service::DeviceManager manager;
    QString error;
    if (!manager.loadFleet(fleetPath, &error)) {
        std::fprintf(stderr, "[FAIL] load fleet: %s\n",
                     error.isEmpty() ? "unknown error" : error.toLocal8Bit().constData());
        return 2;
    }
    if (!error.isEmpty())
        std::fprintf(stderr, "[WARN] %s\n", error.toLocal8Bit().constData());

    const int deviceCount = manager.count();
    std::printf("fleet loaded: %d device(s), connecting...\n", deviceCount);

    QElapsedTimer timer;
    timer.start();
    manager.connectAll();

    while (timer.elapsed() < timeoutMs && !allLinksConnected(manager))
        pumpEvents(50);

    const bool connected = allLinksConnected(manager);
    if (connected)
        pumpEvents(settleMs); // 留一点时间累计入站帧

    int failed = 0;
    const QVector<service::LinkHealth> health = manager.linkHealth();
    std::printf("%-10s %-8s %-10s %-8s %10s %8s\n",
                "device", "link", "protocol", "state", "frames", "age(ms)");
    for (const service::LinkHealth& item : health) {
        const bool ok = item.connected && item.frames > 0;
        if (!ok)
            ++failed;
        std::printf("%-10s %-8s %-10s %-8s %10llu %8lld %s\n",
                    item.deviceId.toLocal8Bit().constData(),
                    item.linkId.toLocal8Bit().constData(),
                    item.protocol.toLocal8Bit().constData(),
                    item.connected ? "online" : "offline",
                    static_cast<unsigned long long>(item.frames),
                    static_cast<long long>(item.lastInboundAgeMs),
                    ok ? "PASS" : "FAIL");
    }

    std::printf("summary: %d link(s), %d failed, elapsed %lld ms -> %s\n",
                health.size(), failed, static_cast<long long>(timer.elapsed()),
                failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}
