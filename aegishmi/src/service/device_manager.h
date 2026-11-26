#pragma once // 头文件保护

#include "core/device_config.h" // 每台设备的通道配置
#include "service/device_summary.h" // 1 秒摘要

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace aegis::test {
class ITestDevice;
}

namespace aegis::service {

class DeviceService;

// =============================================================================
// LinkSpec —— 设备的一条链路（EQ-06 多链路）
//
// role：main = 主链路（波形/摘要/报警）；aux = 辅助链路（仅命令/单点测量）。
// 字段含义与 DeviceSpec 一致，便于界面复用同一套连接参数表单。
// =============================================================================
struct LinkSpec {
    QString id = QStringLiteral("main");
    QString role = QStringLiteral("main");
    QString protocol = QStringLiteral("custom");
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 9527;
    int unitId = 1;
    int baudRate = 115200;
    QString endpoint;
    QString deviceConfigPath;
    bool autoConnect = true;
    QString scpiQuery = QStringLiteral("MEAS:VOLT:DC?");
    int scpiPollMs = 100;
    double scpiScale = 1.0;
    // SCPI 传输：tcp（默认）/ visa / usbtmc。后两者需厂商驱动，未接入时链路不可用。
    QString scpiTransport = QStringLiteral("tcp");

    bool isMain() const
    {
        return role.compare(QStringLiteral("aux"), Qt::CaseInsensitive) != 0;
    }
};

// =============================================================================
// DeviceSpec —— 一台设备的连接参数
//
// protocol 取值：
//   custom    自定义帧协议（TCP，默认 9527）
//   modbusTcp Modbus TCP（端口 + 站号）
//   modbusRtu Modbus RTU（串口名放 host，如 COM3）
//   opcUa     OPC UA（endpoint 为空时用 host 作为 URL）
//   scpi      SCPI 仪器（SCPI-RAW over TCP，默认 5025）
//
// links 为空时，上面的单链路字段视为唯一的主链路（兼容旧 fleet.json）。
// =============================================================================
struct DeviceSpec {
    QString id;                         // 唯一标识，如 "dev1"
    QString name;                       // 显示名称
    QString protocol = QStringLiteral("custom");
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 9527;
    int unitId = 1;                     // Modbus 站号
    int baudRate = 115200;              // Modbus RTU 波特率
    QString endpoint;                   // OPC UA 端点（可选）
    QString deviceConfigPath;           // 通道配置路径（可选；空则用当前 active.json）
    bool autoConnect = true;            // 启动时是否自动连接
    QString scpiQuery = QStringLiteral("MEAS:VOLT:DC?"); // SCPI 周期查询命令
    int scpiPollMs = 100;               // SCPI 轮询周期
    double scpiScale = 1.0;             // 仪器单位 → 通道单位（V→mV 填 1000）
    QString scpiTransport = QStringLiteral("tcp"); // tcp / visa / usbtmc
    QVector<LinkSpec> links;            // 空 = 按上面的单链路字段合成一条主链路
};

// =============================================================================
// LinkHealth —— 单条链路的健康快照（EQ-06 收尾：链路健康面板）
//
// 界面每秒读取一次：在线状态、最近入站延迟、累计帧/错误字节、重连次数。
// =============================================================================
struct LinkHealth {
    QString deviceId;
    QString linkId;
    QString role;       // main / aux
    QString protocol;   // custom / modbusTcp / modbusRtu / opcUa / scpi
    bool connected = false;
    qint64 lastInboundAgeMs = -1; // 最近一次入站距今毫秒；-1 表示从未收到
    std::uint64_t frames = 0;
    std::uint64_t crcErrors = 0;
    std::uint64_t bytes = 0;
    int reconnectAttempts = 0;
};

// =============================================================================
// DeviceManager —— 多设备管理器
//
// 职责：
//   1. 从 fleet.json 读取设备清单（id/协议/地址/通道配置/多链路）；
//   2. 为每条链路创建独立的 DeviceService（各自一条 QThread + 环形缓冲）；
//   3. 把各链路的连接状态/日志聚合为带 deviceId + linkId 的信号，交给界面；
//   4. 提供按设备/按链路连接/断开/全部连接/全部断开。
//
// 线程模型：DeviceManager 与 DeviceService 都住在主线程；
//           真正的网络 IO 在各 DeviceService 内部的通信线程。
// =============================================================================
class DeviceManager : public QObject {
    Q_OBJECT

public:
    explicit DeviceManager(QObject* parent = nullptr);
    ~DeviceManager() override;

    // 读取设备清单。path 为空时按默认搜索顺序查找；找不到则使用内置的 1 台默认设备。
    bool loadFleet(const QString& path = QString(), QString* error = nullptr);

    // 按默认顺序查找 fleet 配置：AEGIS_FLEET_CONFIG → exe/configs/devices/fleet.json → 源码目录。
    static QString defaultFleetPath();

    // 添加一台设备（测试/动态配置用）。id 重复时返回 false。
    // spec.links 为空时按单链路处理。
    bool addDevice(const DeviceSpec& spec, const core::DeviceConfig& config);

    int count() const { return entries_.size(); }
    QStringList ids() const;
    bool contains(const QString& id) const;
    DeviceSpec spec(const QString& id) const;
    core::DeviceConfig config(const QString& id) const;

    // 设备的主链路（UI 波形/测试引擎默认目标）。
    DeviceService* device(const QString& id) const;
    // 指定链路；linkId 为空表示主链路。
    DeviceService* link(const QString& id, const QString& linkId) const;
    QStringList linkIds(const QString& id) const;
    // 主链路 id（多链路设备；单链路返回唯一链路 id）。
    QString mainLinkId(const QString& id) const;
    // 指定链路的参数；linkId 为空表示主链路。
    LinkSpec linkSpec(const QString& id, const QString& linkId) const;

    // 链路健康快照：id 为空时返回全部设备的所有链路。
    QVector<LinkHealth> linkHealth(const QString& id = QString()) const;

    // 测试引擎使用的路由对象：按步骤的 linkId 选择链路，空则主链路。
    // 对象生命周期与设备一致，可直接交给 TestEngine。
    aegis::test::ITestDevice* testDevice(const QString& id) const;

    // 用最新参数连接指定设备的所有链路（连接前会更新 spec）。
    void connectDevice(const QString& id, const DeviceSpec& spec);
    // 按链路连接/断开（界面链路下拉使用）；连接前会更新该链路参数。
    void connectLink(const QString& id, const QString& linkId, const LinkSpec& spec);
    void disconnectLink(const QString& id, const QString& linkId);
    void disconnectDevice(const QString& id);
    void connectAll();
    void disconnectAll();

signals:
    // 某台设备某条链路的连接状态变化。
    void deviceConnectionChanged(const QString& id, const QString& linkId,
                                 bool connected, const QString& detail);
    // 某台设备某条链路的日志。
    void deviceLog(const QString& id, const QString& linkId, const QString& message);
    // 某台设备每秒的波形摘要（趋势存储用，只取主链路）。
    void deviceSummaryReady(const QString& id, const aegis::service::DeviceSummary& summary);
    // 设备清单加载完成（界面据此刷新列表）。
    void fleetLoaded();

private:
    struct LinkEntry {
        LinkSpec spec;
        DeviceService* service = nullptr;
    };
    struct Entry {
        DeviceSpec spec;
        core::DeviceConfig config;
        QVector<LinkEntry> links;
        aegis::test::ITestDevice* router = nullptr;
    };

    Entry* find(const QString& id);
    const Entry* find(const QString& id) const;
    static LinkEntry* findLink(Entry* entry, const QString& linkId);
    static const LinkEntry* findLink(const Entry* entry, const QString& linkId);
    static void applySpec(DeviceService* service, const LinkSpec& spec);
    static core::DeviceConfig loadDeviceConfig(const QString& path);

    QVector<Entry> entries_;
};

} // namespace aegis::service
