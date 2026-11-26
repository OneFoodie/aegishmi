#pragma once // 头文件保护

#include <QMetaType>
#include <QVector>
#include <QtGlobal>

namespace aegis::service {

// =============================================================================
// 波形摘要（1 秒粒度）
//
// 为什么需要：原始波形 10 kHz × N 通道无法长期保存（一天就是几十 GB）。
// 上位机按秒把每个通道聚合成 min/max/avg/count 四个量：
//   - 趋势查询只读这张表，几小时/几天的数据也只有几千行；
//   - min/max 保留毛刺，avg 体现中心趋势；
//   - count 用于判断该秒是否掉线（明显偏小说明丢数据）。
// =============================================================================
struct ChannelSummary {
    double minValue = 0.0;
    double maxValue = 0.0;
    double avgValue = 0.0;
    int sampleCount = 0;
};

struct DeviceSummary {
    qint64 epochSec = 0;              // 这一秒的起始时间（Unix 秒）
    QVector<ChannelSummary> channels; // 下标 = 通道号
};

} // namespace aegis::service

// 跨线程队列信号需要注册；放在头文件里便于各模块统一。
Q_DECLARE_METATYPE(aegis::service::DeviceSummary)
