#pragma once // 头文件保护

#include "core/device_config.h"     // DeviceChannel 阈值字段
#include "service/device_summary.h"
#include "storage/system_repository.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace aegis::service {

class DeviceManager;

// =============================================================================
// AlarmMonitor —— 通道阈值报警
//
// 判定依据：设备每秒摘要里的 avg（1 秒粒度）。
//   - 阈值来自 DeviceChannel 的 high / low（可选）；
//   - 进入报警：avg 越过阈值；
//   - 退出报警：avg 回到阈值内侧至少 deadband（回差），避免在阈值附近抖动刷屏；
//   - 每次"进入 → 恢复"在数据库里对应一条 alarm_event。
//
// 线程模型：只消费 DeviceManager 的 deviceSummaryReady（主线程），不额外开线程。
// =============================================================================
class AlarmMonitor : public QObject {
    Q_OBJECT

public:
    AlarmMonitor(DeviceManager* manager, aegis::storage::SystemRepository* repository,
                 QObject* parent = nullptr);

    // 当前所有设备未恢复的报警总数。
    int openCount() const { return openCount_; }

    // ISA-18.2 搁置（shelve）：指定通道在 minutes 内不再产生新报警。
    // 已存在的报警仍按恢复条件正常清除；到期后若仍越限会重新报警。
    void shelve(const QString& deviceId, int channel, int minutes);
    bool isShelved(const QString& deviceId, int channel) const;

public slots:
    // 消费一台设备的一秒摘要。
    void onSummary(const QString& deviceId, const aegis::service::DeviceSummary& summary);

signals:
    // 新进入一条报警（已写入数据库，id 有效；写入失败时 id 为 0）。
    void alarmRaised(const aegis::storage::AlarmRecord& alarm);
    // 某条报警已恢复（alarmId 对应 alarmRaised 里的 id）。
    void alarmCleared(int alarmId, const QString& deviceId);

private:
    struct ChannelState {
        int level = 0;    // 0 正常 / +1 高报警 / -1 低报警
        int alarmId = 0;  // 当前未恢复报警的记录 id
    };

    void raiseChannel(const QString& deviceId, const QString& deviceName,
                      int channel, const aegis::core::DeviceChannel& meta,
                      double value, int level, ChannelState& state);
    void clearChannel(const QString& deviceId, ChannelState& state);

    DeviceManager* manager_ = nullptr;
    aegis::storage::SystemRepository* repository_ = nullptr;
    QHash<QString, QVector<ChannelState>> states_; // 设备 -> 每通道状态
    QHash<QString, qint64> shelvedUntil_;          // "device#channel" -> 到期毫秒
    int openCount_ = 0;
};

} // namespace aegis::service
