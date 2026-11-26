#include "service/device_manager.h"

#include "service/device_service.h"
#include "test/test_device.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace aegis::service {

namespace {

// 用设备级单链路字段合成一条主链路（旧 fleet.json 兼容路径）。
LinkSpec linkFromSpec(const DeviceSpec& spec)
{
    LinkSpec link;
    link.id = QStringLiteral("main");
    link.role = QStringLiteral("main");
    link.protocol = spec.protocol;
    link.host = spec.host;
    link.port = spec.port;
    link.unitId = spec.unitId;
    link.baudRate = spec.baudRate;
    link.endpoint = spec.endpoint;
    link.deviceConfigPath = spec.deviceConfigPath;
    link.autoConnect = spec.autoConnect;
    link.scpiQuery = spec.scpiQuery;
    link.scpiPollMs = spec.scpiPollMs;
    link.scpiScale = spec.scpiScale;
    link.scpiTransport = spec.scpiTransport;
    return link;
}

// 把链路参数回写到设备级字段，供界面按旧方式展示主链路。
void syncSpecFromMain(DeviceSpec* spec, const QVector<LinkSpec>& links)
{
    for (const LinkSpec& link : links) {
        if (!link.isMain())
            continue;
        spec->protocol = link.protocol;
        spec->host = link.host;
        spec->port = link.port;
        spec->unitId = link.unitId;
        spec->baudRate = link.baudRate;
        spec->endpoint = link.endpoint;
        spec->deviceConfigPath = link.deviceConfigPath;
        spec->autoConnect = link.autoConnect;
        spec->scpiQuery = link.scpiQuery;
        spec->scpiPollMs = link.scpiPollMs;
        spec->scpiScale = link.scpiScale;
        spec->scpiTransport = link.scpiTransport;
        break;
    }
}

LinkSpec parseLink(const QJsonObject& object, const QDir& baseDir, const QString& defaultId)
{
    LinkSpec link;
    link.id = object.value(QStringLiteral("id")).toString(defaultId);
    link.role = object.value(QStringLiteral("role")).toString(QStringLiteral("main"));
    link.protocol = object.value(QStringLiteral("protocol")).toString(
        QStringLiteral("custom"));
    link.host = object.value(QStringLiteral("host")).toString(QStringLiteral("127.0.0.1"));
    link.port = static_cast<quint16>(object.value(QStringLiteral("port")).toInt(9527));
    link.unitId = object.value(QStringLiteral("unitId")).toInt(1);
    link.baudRate = object.value(QStringLiteral("baudRate")).toInt(115200);
    link.endpoint = object.value(QStringLiteral("endpoint")).toString();
    link.autoConnect = object.value(QStringLiteral("autoConnect")).toBool(true);
    link.scpiQuery = object.value(QStringLiteral("scpiQuery")).toString(
        QStringLiteral("MEAS:VOLT:DC?"));
    link.scpiPollMs = object.value(QStringLiteral("scpiPollMs")).toInt(100);
    link.scpiScale = object.value(QStringLiteral("scpiScale")).toDouble(1.0);
    link.scpiTransport = object.value(QStringLiteral("transport")).toString(
        QStringLiteral("tcp"));

    QString configPath = object.value(QStringLiteral("config")).toString();
    if (!configPath.isEmpty() && QDir::isRelativePath(configPath))
        configPath = baseDir.absoluteFilePath(configPath);
    link.deviceConfigPath = configPath;
    return link;
}

// 测试引擎的路由对象：按步骤的 linkId 选择链路，空则主链路。
class LinkRouter final : public aegis::test::ITestDevice {
public:
    LinkRouter(DeviceManager* manager, QString deviceId)
        : manager_(manager)
        , deviceId_(std::move(deviceId))
    {
    }

    bool sendCommand(test::DeviceCommand command, int channel, int value,
                     int timeoutMs, quint8& status,
                     const QString& linkId) override
    {
        DeviceService* service = manager_->link(deviceId_, linkId);
        if (service == nullptr)
            return false;
        return service->sendCommand(command, channel, value, timeoutMs, status, linkId);
    }

    bool queryText(const QString& cmd, int timeoutMs, QString* value,
                   QString* errorMessage, const QString& linkId) override
    {
        DeviceService* service = manager_->link(deviceId_, linkId);
        if (service == nullptr) {
            if (errorMessage != nullptr)
                *errorMessage = QStringLiteral("链路不存在：%1").arg(linkId);
            return false;
        }
        return service->queryText(cmd, timeoutMs, value, errorMessage, linkId);
    }

    std::vector<std::int16_t> sampleWindow(int channel, int windowMs,
                                           const QString& linkId) override
    {
        DeviceService* service = manager_->link(deviceId_, linkId);
        return service != nullptr
            ? service->sampleWindow(channel, windowMs, linkId)
            : std::vector<std::int16_t>{};
    }

    int sampleRateHz(const QString& linkId) const override
    {
        DeviceService* service = manager_->link(deviceId_, linkId);
        return service != nullptr ? service->sampleRateHz(linkId) : 0;
    }

private:
    DeviceManager* manager_ = nullptr;
    QString deviceId_;
};

} // namespace

DeviceManager::DeviceManager(QObject* parent)
    : QObject(parent)
{
}

DeviceManager::~DeviceManager()
{
    // DeviceService 析构会停掉自己的工作线程，这里按顺序释放即可。
    for (Entry& entry : entries_) {
        for (LinkEntry& link : entry.links) {
            delete link.service;
            link.service = nullptr;
        }
        delete entry.router;
        entry.router = nullptr;
    }
    entries_.clear();
}

QString DeviceManager::defaultFleetPath()
{
    QStringList candidates;

    const QString fromEnv = qEnvironmentVariable("AEGIS_FLEET_CONFIG");
    if (!fromEnv.isEmpty())
        candidates.append(fromEnv);

    const QString appDir = QCoreApplication::applicationDirPath();
    candidates.append(appDir + QStringLiteral("/configs/devices/fleet.json"));
    candidates.append(appDir + QStringLiteral("/../../configs/devices/fleet.json"));

    for (const QString& path : candidates) {
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}

core::DeviceConfig DeviceManager::loadDeviceConfig(const QString& path)
{
    if (path.isEmpty())
        return core::DeviceConfig::loadActive();

    QFileInfo info(path);
    if (!info.exists())
        return core::DeviceConfig::loadActive();

    return core::DeviceConfig::load(path);
}

bool DeviceManager::loadFleet(const QString& path, QString* error)
{
    // 重新加载前先释放旧设备（会停止各自线程）。
    for (Entry& entry : entries_) {
        for (LinkEntry& link : entry.links) {
            delete link.service;
            link.service = nullptr;
        }
        delete entry.router;
        entry.router = nullptr;
    }
    entries_.clear();

    QString fleetPath = path;
    if (fleetPath.isEmpty())
        fleetPath = defaultFleetPath();

    if (fleetPath.isEmpty() || !QFileInfo::exists(fleetPath)) {
        // 没有清单：退化为 1 台默认设备，保持单设备模式可用。
        DeviceSpec spec;
        spec.id = QStringLiteral("dev1");
        spec.name = QStringLiteral("设备-1");
        spec.protocol = QStringLiteral("custom");
        spec.host = QStringLiteral("127.0.0.1");
        spec.port = 9527;
        addDevice(spec, core::DeviceConfig::loadActive());
        if (error != nullptr && !fleetPath.isEmpty())
            *error = QStringLiteral("设备清单不存在：%1").arg(fleetPath);
        emit fleetLoaded();
        return !entries_.isEmpty();
    }

    QFile file(fleetPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("无法打开设备清单：%1").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("设备清单 JSON 解析失败：%1").arg(parseError.errorString());
        return false;
    }

    const QDir baseDir = QFileInfo(fleetPath).absoluteDir();
    const QJsonArray devices = document.object().value(QStringLiteral("devices")).toArray();
    if (devices.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("设备清单为空（缺少 devices 数组）");
        return false;
    }

    int index = 0;
    for (const QJsonValue& value : devices) {
        const QJsonObject object = value.toObject();
        DeviceSpec spec;
        spec.id = object.value(QStringLiteral("id")).toString(
            QStringLiteral("dev%1").arg(++index));
        spec.name = object.value(QStringLiteral("name")).toString(spec.id);
        spec.protocol = object.value(QStringLiteral("protocol")).toString(
            QStringLiteral("custom"));
        spec.host = object.value(QStringLiteral("host")).toString(
            QStringLiteral("127.0.0.1"));
        spec.port = static_cast<quint16>(
            object.value(QStringLiteral("port")).toInt(9527));
        spec.unitId = object.value(QStringLiteral("unitId")).toInt(1);
        spec.baudRate = object.value(QStringLiteral("baudRate")).toInt(115200);
        spec.endpoint = object.value(QStringLiteral("endpoint")).toString();
        spec.autoConnect = object.value(QStringLiteral("autoConnect")).toBool(true);
        spec.scpiQuery = object.value(QStringLiteral("scpiQuery")).toString(
            QStringLiteral("MEAS:VOLT:DC?"));
        spec.scpiPollMs = object.value(QStringLiteral("scpiPollMs")).toInt(100);
        spec.scpiScale = object.value(QStringLiteral("scpiScale")).toDouble(1.0);
        spec.scpiTransport = object.value(QStringLiteral("transport")).toString(
            QStringLiteral("tcp"));

        // 通道配置路径：相对 fleet.json 所在目录解析，便于整包移动。
        QString configPath = object.value(QStringLiteral("config")).toString();
        if (!configPath.isEmpty() && QDir::isRelativePath(configPath))
            configPath = baseDir.absoluteFilePath(configPath);
        spec.deviceConfigPath = configPath;

        // 多链路：links 为空时按旧单链路字段合成一条主链路。
        const QJsonArray links = object.value(QStringLiteral("links")).toArray();
        if (links.isEmpty()) {
            spec.links = {linkFromSpec(spec)};
        } else {
            QSet<QString> seen;
            int linkIndex = 0;
            for (const QJsonValue& linkValue : links) {
                LinkSpec link = parseLink(linkValue.toObject(), baseDir,
                                          QStringLiteral("link%1").arg(++linkIndex));
                if (link.id.isEmpty() || seen.contains(link.id))
                    continue;
                seen.insert(link.id);
                spec.links.append(link);
            }
            if (spec.links.isEmpty())
                spec.links = {linkFromSpec(spec)};
            syncSpecFromMain(&spec, spec.links);
        }

        addDevice(spec, loadDeviceConfig(spec.deviceConfigPath));
    }

    emit fleetLoaded();
    return !entries_.isEmpty();
}

bool DeviceManager::addDevice(const DeviceSpec& spec, const core::DeviceConfig& config)
{
    if (spec.id.isEmpty() || contains(spec.id))
        return false;

    Entry entry;
    entry.spec = spec;
    entry.config = config;

    const QVector<LinkSpec> links = spec.links.isEmpty()
        ? QVector<LinkSpec>{linkFromSpec(spec)}
        : spec.links;
    entry.spec.links = links;
    syncSpecFromMain(&entry.spec, links);

    const QString id = spec.id;
    for (const LinkSpec& linkSpec : links) {
        LinkEntry link;
        link.spec = linkSpec;

        const core::DeviceConfig linkConfig = linkSpec.deviceConfigPath == spec.deviceConfigPath
            ? config
            : loadDeviceConfig(linkSpec.deviceConfigPath);
        link.service = new DeviceService(this, linkConfig);

        const QString linkId = linkSpec.id;
        const bool isMain = linkSpec.isMain();
        connect(link.service, &DeviceService::connectedChanged, this,
                [this, id, linkId](bool connected, const QString& detail) {
                    emit deviceConnectionChanged(id, linkId, connected, detail);
                });
        connect(link.service, &DeviceService::logMessage, this,
                [this, id, linkId](const QString& message) {
                    emit deviceLog(id, linkId, message);
                });
        if (isMain) {
            connect(link.service, &DeviceService::summaryReady, this,
                    [this, id](const aegis::service::DeviceSummary& summary) {
                        emit deviceSummaryReady(id, summary);
                    });
        }

        entry.links.append(link);
    }

    entry.router = new LinkRouter(this, id);
    entries_.append(entry);
    return true;
}

QStringList DeviceManager::ids() const
{
    QStringList result;
    result.reserve(entries_.size());
    for (const Entry& entry : entries_)
        result.append(entry.spec.id);
    return result;
}

bool DeviceManager::contains(const QString& id) const
{
    return find(id) != nullptr;
}

DeviceSpec DeviceManager::spec(const QString& id) const
{
    if (const Entry* entry = find(id))
        return entry->spec;
    return {};
}

core::DeviceConfig DeviceManager::config(const QString& id) const
{
    if (const Entry* entry = find(id))
        return entry->config;
    return {};
}

DeviceService* DeviceManager::device(const QString& id) const
{
    if (const Entry* entry = find(id)) {
        const LinkEntry* link = findLink(entry, QString());
        return link != nullptr ? link->service : nullptr;
    }
    return nullptr;
}

DeviceService* DeviceManager::link(const QString& id, const QString& linkId) const
{
    if (const Entry* entry = find(id)) {
        const LinkEntry* link = findLink(entry, linkId);
        return link != nullptr ? link->service : nullptr;
    }
    return nullptr;
}

QStringList DeviceManager::linkIds(const QString& id) const
{
    QStringList result;
    if (const Entry* entry = find(id)) {
        result.reserve(entry->links.size());
        for (const LinkEntry& link : entry->links)
            result.append(link.spec.id);
    }
    return result;
}

QString DeviceManager::mainLinkId(const QString& id) const
{
    if (const Entry* entry = find(id)) {
        const LinkEntry* link = findLink(entry, QString());
        if (link != nullptr)
            return link->spec.id;
    }
    return {};
}

LinkSpec DeviceManager::linkSpec(const QString& id, const QString& linkId) const
{
    if (const Entry* entry = find(id)) {
        const LinkEntry* link = findLink(entry, linkId);
        if (link != nullptr)
            return link->spec;
    }
    return {};
}

QVector<LinkHealth> DeviceManager::linkHealth(const QString& id) const
{
    QVector<LinkHealth> result;
    for (const Entry& entry : entries_) {
        if (!id.isEmpty() && entry.spec.id != id)
            continue;
        for (const LinkEntry& link : entry.links) {
            if (link.service == nullptr)
                continue;

            LinkHealth health;
            health.deviceId = entry.spec.id;
            health.linkId = link.spec.id;
            health.role = link.spec.role;
            health.protocol = link.spec.protocol;
            health.connected = link.service->isConnected();
            health.lastInboundAgeMs = link.service->lastInboundAgeMs();
            health.frames = link.service->stats().frames;
            health.crcErrors = link.service->stats().crcErrors;
            health.bytes = link.service->stats().bytes;
            health.reconnectAttempts = link.service->reconnectAttempts();
            result.append(health);
        }
    }
    return result;
}

aegis::test::ITestDevice* DeviceManager::testDevice(const QString& id) const
{
    if (const Entry* entry = find(id))
        return entry->router;
    return nullptr;
}

void DeviceManager::connectDevice(const QString& id, const DeviceSpec& spec)
{
    Entry* entry = find(id);
    if (entry == nullptr)
        return;

    // 地址/站号等参数以最新传入的为准；通道配置保持不变。
    entry->spec = spec;

    if (!spec.links.isEmpty()) {
        // 配置显式给出链路：按 id 更新已有链路，缺失的补建。
        for (const LinkSpec& linkSpec : spec.links) {
            LinkEntry* link = findLink(entry, linkSpec.id);
            if (link != nullptr) {
                link->spec = linkSpec;
            } else {
                LinkEntry added;
                added.spec = linkSpec;
                added.service = new DeviceService(this, entry->config);
                const QString deviceId = id;
                const QString linkId = linkSpec.id;
                const bool isMain = linkSpec.isMain();
                connect(added.service, &DeviceService::connectedChanged, this,
                        [this, deviceId, linkId](bool connected, const QString& detail) {
                            emit deviceConnectionChanged(deviceId, linkId, connected, detail);
                        });
                connect(added.service, &DeviceService::logMessage, this,
                        [this, deviceId, linkId](const QString& message) {
                            emit deviceLog(deviceId, linkId, message);
                        });
                if (isMain) {
                    connect(added.service, &DeviceService::summaryReady, this,
                            [this, deviceId](const aegis::service::DeviceSummary& summary) {
                                emit deviceSummaryReady(deviceId, summary);
                            });
                }
                entry->links.append(added);
            }
        }
    } else if (!entry->links.isEmpty()) {
        // 旧式单链路参数：只更新主链路，保留辅助链路。
        LinkSpec mainSpec = linkFromSpec(spec);
        LinkEntry* mainLink = findLink(entry, QString());
        if (mainLink != nullptr) {
            mainSpec.id = mainLink->spec.id;
            mainSpec.role = mainLink->spec.role;
            mainLink->spec = mainSpec;
        }
        QVector<LinkSpec> merged;
        for (const LinkEntry& link : entry->links)
            merged.append(link.spec);
        entry->spec.links = merged;
        syncSpecFromMain(&entry->spec, merged);
    }

    for (LinkEntry& link : entry->links) {
        if (link.spec.autoConnect)
            applySpec(link.service, link.spec);
    }
}

void DeviceManager::connectLink(const QString& id, const QString& linkId, const LinkSpec& spec)
{
    Entry* entry = find(id);
    if (entry == nullptr)
        return;

    LinkEntry* link = findLink(entry, linkId);
    if (link == nullptr || link->service == nullptr)
        return;

    link->spec = spec;
    link->spec.id = linkId.isEmpty() ? link->spec.id : linkId;
    applySpec(link->service, link->spec);
}

void DeviceManager::disconnectLink(const QString& id, const QString& linkId)
{
    Entry* entry = find(id);
    if (entry == nullptr)
        return;
    if (LinkEntry* link = findLink(entry, linkId)) {
        if (link->service != nullptr)
            link->service->disconnectDevice();
    }
}

void DeviceManager::disconnectDevice(const QString& id)
{
    if (Entry* entry = find(id)) {
        for (LinkEntry& link : entry->links) {
            if (link.service != nullptr)
                link.service->disconnectDevice();
        }
    }
}

void DeviceManager::connectAll()
{
    for (const Entry& entry : entries_) {
        for (const LinkEntry& link : entry.links) {
            if (link.spec.autoConnect)
                applySpec(link.service, link.spec);
        }
    }
}

void DeviceManager::disconnectAll()
{
    for (Entry& entry : entries_) {
        for (LinkEntry& link : entry.links) {
            if (link.service != nullptr)
                link.service->disconnectDevice();
        }
    }
}

void DeviceManager::applySpec(DeviceService* service, const LinkSpec& spec)
{
    if (service == nullptr)
        return;

    if (spec.protocol == QLatin1String("modbusTcp")) {
        service->connectModbusTcp(spec.host, spec.port, spec.unitId);
    } else if (spec.protocol == QLatin1String("modbusRtu")) {
        service->connectModbusRtu(spec.host, spec.baudRate, spec.unitId);
    } else if (spec.protocol == QLatin1String("opcUa")) {
        const QString endpoint = spec.endpoint.isEmpty()
            ? QStringLiteral("opc.tcp://%1:%2").arg(spec.host).arg(spec.port)
            : spec.endpoint;
        service->connectOpcUa(endpoint);
    } else if (spec.protocol == QLatin1String("scpi")) {
        if (spec.scpiTransport.compare(QLatin1String("tcp"), Qt::CaseInsensitive) != 0) {
            // GPIB/USBTMC 需要厂商 VISA 驱动；未接入时让该链路明确不可用，
            // 不影响其它链路与主程序运行。
            service->reportLinkUnavailable(
                QStringLiteral("SCPI 传输 %1 需要厂商驱动，当前版本未启用（%2）")
                    .arg(spec.scpiTransport, spec.host));
            return;
        }
        service->connectScpi(spec.host, spec.port, spec.scpiQuery,
                             spec.scpiPollMs, spec.scpiScale);
    } else {
        service->connectTcp(spec.host, spec.port);
    }
}

DeviceManager::Entry* DeviceManager::find(const QString& id)
{
    for (Entry& entry : entries_) {
        if (entry.spec.id == id)
            return &entry;
    }
    return nullptr;
}

const DeviceManager::Entry* DeviceManager::find(const QString& id) const
{
    for (const Entry& entry : entries_) {
        if (entry.spec.id == id)
            return &entry;
    }
    return nullptr;
}

DeviceManager::LinkEntry* DeviceManager::findLink(Entry* entry, const QString& linkId)
{
    if (entry == nullptr)
        return nullptr;
    for (LinkEntry& link : entry->links) {
        if (linkId.isEmpty() ? link.spec.isMain() : link.spec.id == linkId)
            return &link;
    }
    return nullptr;
}

const DeviceManager::LinkEntry* DeviceManager::findLink(const Entry* entry, const QString& linkId)
{
    if (entry == nullptr)
        return nullptr;
    for (const LinkEntry& link : entry->links) {
        if (linkId.isEmpty() ? link.spec.isMain() : link.spec.id == linkId)
            return &link;
    }
    return nullptr;
}

} // namespace aegis::service
