#include "service/alarm_monitor.h"

#include "core/device_config.h"
#include "service/device_manager.h"

#include <QDateTime>

namespace aegis::service {

AlarmMonitor::AlarmMonitor(DeviceManager* manager,
                           aegis::storage::SystemRepository* repository,
                           QObject* parent)
    : QObject(parent)
    , manager_(manager)
    , repository_(repository)
{
    // 上一次运行遗留的未恢复报警不属于本次监控会话：先标记恢复，
    // 避免重启后同一通道出现"两条未恢复记录"；若条件仍越限，下一秒会重新报警。
    if (repository_ != nullptr) {
        const QVector<aegis::storage::AlarmRecord> stale =
            repository_->recentAlarms(1000, true);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (const aegis::storage::AlarmRecord& alarm : stale)
            repository_->clearAlarm(alarm.id, now);
    }
}

void AlarmMonitor::onSummary(const QString& deviceId,
                             const aegis::service::DeviceSummary& summary)
{
    if (manager_ == nullptr)
        return;

    const core::DeviceConfig config = manager_->config(deviceId);
    const QString deviceName = manager_->spec(deviceId).name;
    const int channelCount = config.channelCount;
    if (channelCount <= 0)
        return;

    QVector<ChannelState>& states = states_[deviceId];
    if (states.size() != channelCount)
        states.resize(channelCount);

    // 摘要与配置取较小值，防御配置热切换时的越界。
    const int count = qMin(summary.channels.size(), channelCount);
    for (int ch = 0; ch < count; ++ch) {
        const core::DeviceChannel& meta = config.channels.at(ch);
        if (!meta.hasHigh && !meta.hasLow)
            continue;

        const ChannelSummary& channel = summary.channels.at(ch);
        if (channel.sampleCount <= 0)
            continue;

        const double value = channel.avgValue;
        ChannelState& state = states[ch];
        const int previous = state.level;
        int next = previous;

        if (previous == 0) {
            // 正常态：越过阈值才进入报警。
            if (meta.hasHigh && value > meta.high)
                next = 1;
            else if (meta.hasLow && value < meta.low)
                next = -1;
        } else if (previous > 0) {
            // 高报警：回落到 high - deadband 以内才恢复；恢复后允许直接转低报警。
            if (!meta.hasHigh || value <= meta.high - meta.deadband) {
                next = 0;
                if (meta.hasLow && value < meta.low)
                    next = -1;
            }
        } else {
            // 低报警：回升到 low + deadband 以上才恢复。
            if (!meta.hasLow || value >= meta.low + meta.deadband) {
                next = 0;
                if (meta.hasHigh && value > meta.high)
                    next = 1;
            }
        }

        if (next == previous)
            continue;

        if (previous != 0)
            clearChannel(deviceId, state);
        // 搁置期内只抑制"新报警"，恢复仍照常处理，避免留下悬挂的未恢复记录。
        if (next != 0 && !isShelved(deviceId, ch))
            raiseChannel(deviceId, deviceName, ch, meta, value, next, state);
    }
}

void AlarmMonitor::shelve(const QString& deviceId, int channel, int minutes)
{
    const qint64 until = QDateTime::currentMSecsSinceEpoch()
        + static_cast<qint64>(qMax(1, minutes)) * 60 * 1000;
    shelvedUntil_.insert(deviceId + QLatin1Char('#') + QString::number(channel), until);
}

bool AlarmMonitor::isShelved(const QString& deviceId, int channel) const
{
    const auto it = shelvedUntil_.constFind(deviceId + QLatin1Char('#')
                                            + QString::number(channel));
    return it != shelvedUntil_.constEnd()
        && it.value() > QDateTime::currentMSecsSinceEpoch();
}

void AlarmMonitor::raiseChannel(const QString& deviceId, const QString& deviceName,
                                int channel, const aegis::core::DeviceChannel& meta,
                                double value, int level, ChannelState& state)
{
    aegis::storage::AlarmRecord alarm;
    alarm.deviceId = deviceId;
    alarm.deviceName = deviceName;
    alarm.channel = channel;
    alarm.channelName = meta.name.isEmpty()
                             ? QStringLiteral("CH%1").arg(channel)
                             : meta.name;
    alarm.unit = meta.unit;
    alarm.level = level > 0 ? QStringLiteral("HIGH") : QStringLiteral("LOW");
    alarm.value = value;
    alarm.threshold = level > 0 ? meta.high : meta.low;
    alarm.raisedAtMs = QDateTime::currentMSecsSinceEpoch();

    if (repository_ != nullptr)
        alarm.id = repository_->saveAlarm(alarm);

    state.level = level;
    state.alarmId = alarm.id;
    ++openCount_;
    emit alarmRaised(alarm);
}

void AlarmMonitor::clearChannel(const QString& deviceId, ChannelState& state)
{
    const int alarmId = state.alarmId;
    state.alarmId = 0;
    state.level = 0;

    if (alarmId > 0 && repository_ != nullptr)
        repository_->clearAlarm(alarmId, QDateTime::currentMSecsSinceEpoch());

    if (openCount_ > 0)
        --openCount_;
    if (alarmId > 0)
        emit alarmCleared(alarmId, deviceId);
}

} // namespace aegis::service
