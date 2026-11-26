#include "core/device_config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace aegis::core {

namespace {

// 把 JSON 里的数字钳制到合法范围。
int clampInt(int value, int minValue, int maxValue)
{
    return std::clamp(value, minValue, maxValue);
}

} // namespace

DeviceConfig DeviceConfig::load(const QString& path, QString* error)
{
    DeviceConfig config;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("无法打开设备配置：%1").arg(file.errorString());
        return config;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("设备配置 JSON 解析失败：%1").arg(parseError.errorString());
        return config;
    }

    const QJsonObject root = document.object();
    config.channelCount = clampInt(root.value(QStringLiteral("channelCount"))
                                       .toInt(kDefaultChannelCount),
                                   1, kMaxChannels);
    config.sampleRateHz = clampInt(root.value(QStringLiteral("sampleRateHz"))
                                       .toInt(kDefaultSampleRateHz),
                                   100, 1000000);

    const QJsonArray channels = root.value(QStringLiteral("channels")).toArray();
    for (const QJsonValue& value : channels) {
        const QJsonObject object = value.toObject();
        DeviceChannel channel;
        channel.name = object.value(QStringLiteral("name")).toString();
        channel.unit = object.value(QStringLiteral("unit")).toString();
        channel.description = object.value(QStringLiteral("description")).toString();

        // 报警阈值：缺省表示不做阈值判断；deadband 只对已配置的阈值生效。
        if (object.contains(QStringLiteral("high"))) {
            channel.hasHigh = true;
            channel.high = object.value(QStringLiteral("high")).toDouble(0.0);
        }
        if (object.contains(QStringLiteral("low"))) {
            channel.hasLow = true;
            channel.low = object.value(QStringLiteral("low")).toDouble(0.0);
        }
        if (object.contains(QStringLiteral("deadband"))) {
            channel.deadband = object.value(QStringLiteral("deadband")).toDouble(0.0);
            if (channel.deadband < 0.0)
                channel.deadband = 0.0;
        }

        // 校准：线性增益/偏置（默认 1 / 0，等于不校准）。
        if (object.contains(QStringLiteral("gain")))
            channel.gain = object.value(QStringLiteral("gain")).toDouble(1.0);
        if (object.contains(QStringLiteral("offset")))
            channel.offset = object.value(QStringLiteral("offset")).toDouble(0.0);

        // 日常点检：配置了 check 才参与点检。
        if (object.contains(QStringLiteral("check"))) {
            channel.hasCheck = true;
            channel.checkValue = object.value(QStringLiteral("check")).toDouble(0.0);
            channel.checkTolerance =
                object.value(QStringLiteral("checkTolerance")).toDouble(0.0);
            if (channel.checkTolerance < 0.0)
                channel.checkTolerance = 0.0;
        }
        config.channels.append(channel);
    }

    config.normalize();
    return config;
}

DeviceConfig DeviceConfig::loadActive()
{
    QStringList candidates;

    // 1) 环境变量优先（便于同一份程序切换 6 通道 / 32 通道演示）。
    const QString fromEnv = qEnvironmentVariable("AEGIS_DEVICE_CONFIG");
    if (!fromEnv.isEmpty())
        candidates.append(fromEnv);

    // 2) dist 形态：exe 同目录/configs/devices/active.json
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates.append(appDir + QStringLiteral("/configs/devices/active.json"));
    // 3) 开发形态：build-mingw/bin 往上两级就是 aegishmi
    candidates.append(appDir + QStringLiteral("/../../configs/devices/active.json"));

    for (const QString& path : candidates) {
        if (QFileInfo::exists(path))
            return load(path);
    }

    // 4) 全部缺失：使用默认 6 通道 / 10 kHz。
    DeviceConfig config;
    config.normalize();
    return config;
}

DeviceConfig DeviceConfig::make(int channelCount, int sampleRateHz)
{
    DeviceConfig config;
    config.channelCount = channelCount;
    config.sampleRateHz = sampleRateHz;
    config.normalize();
    return config;
}

QString DeviceConfig::channelName(int index) const
{
    if (index >= 0 && index < channels.size() && !channels.at(index).name.isEmpty())
        return channels.at(index).name;
    return QStringLiteral("CH%1").arg(index);
}

QString DeviceConfig::channelUnit(int index) const
{
    if (index >= 0 && index < channels.size())
        return channels.at(index).unit;
    return QString();
}

bool DeviceConfig::hasCalibration() const
{
    for (const DeviceChannel& channel : channels) {
        if (std::abs(channel.gain - 1.0) > 1e-9 || std::abs(channel.offset) > 1e-9)
            return true;
    }
    return false;
}

void DeviceConfig::normalize()
{
    channelCount = clampInt(channelCount, 1, kMaxChannels);
    sampleRateHz = clampInt(sampleRateHz, 100, 1000000);

    // 配置里没写全的通道自动补齐，保证界面/协议层可以直接按索引访问。
    while (channels.size() < channelCount)
        channels.append({QStringLiteral("CH%1").arg(channels.size()), QString(), QString()});
    if (channels.size() > channelCount)
        channels.resize(channelCount);
}

} // namespace aegis::core
