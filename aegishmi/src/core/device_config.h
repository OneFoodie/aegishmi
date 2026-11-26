#pragma once // 头文件保护

#include <QString>
#include <QVector>

namespace aegis::core {

// =============================================================================
// DeviceConfig —— 设备通道配置
//
// 目的：把"通道数、采样率、通道名称/单位"从代码常量变成配置项，
//       同一套程序可以接 6 通道电源模块，也可以接 32/64 通道采集卡。
//
// 配置文件示例（configs/devices/active.json）：
// {
//   "schemaVersion": 1,
//   "channelCount": 32,
//   "sampleRateHz": 10000,
//   "channels": [ { "name": "CH0", "unit": "mV", "description": "输出电压" }, ... ]
// }
//
// 读取顺序：
//   1. 环境变量 AEGIS_DEVICE_CONFIG 指定的文件；
//   2. 可执行文件同目录 configs/devices/active.json（dist 部署形态）；
//   3. 可执行文件上两级目录 configs/devices/active.json（build/bin 开发形态）；
//   4. 都没有则使用默认值（6 通道 / 10 kHz）。
// =============================================================================
struct DeviceChannel {
    QString name;        // 显示名称，如 CH0
    QString unit;        // 单位，如 mV / mA / ℃
    QString description; // 说明（可选）

    // ---- 报警阈值（可选；不配置表示该通道不做阈值报警）----
    bool hasHigh = false; // 是否配置了上限
    double high = 0.0;    // 高报警阈值
    bool hasLow = false;  // 是否配置了下限
    double low = 0.0;     // 低报警阈值
    double deadband = 0.0; // 回差：越过阈值后需回到"阈值 ∓ deadband"才恢复

    // ---- 校准（线性补偿：value = raw × gain + offset）----
    double gain = 1.0;
    double offset = 0.0;

    // ---- 日常点检（金样值）----
    bool hasCheck = false;      // 是否参与日常点检
    double checkValue = 0.0;    // 金样/参考值
    double checkTolerance = 0.0; // 允许偏差（绝对值）
};

struct DeviceConfig {
    static constexpr int kDefaultChannelCount = 6;    // 默认通道数（与原实现一致）
    static constexpr int kMaxChannels = 64;           // 上限：够中度规模使用，避免误配置撑爆内存
    static constexpr int kDefaultSampleRateHz = 10000; // 默认采样率

    int channelCount = kDefaultChannelCount; // 通道数（1~kMaxChannels）
    int sampleRateHz = kDefaultSampleRateHz; // 采样率（Hz）
    QVector<DeviceChannel> channels;         // 通道元数据；长度不足时自动补 CH%1

    // 读取指定配置文件；失败时返回默认配置，并通过 error 返回原因。
    static DeviceConfig load(const QString& path, QString* error = nullptr);

    // 按上面的搜索顺序读取"当前生效"的配置。
    static DeviceConfig loadActive();

    // 构造一个简单配置（测试/模拟器常用），自动补齐通道名。
    static DeviceConfig make(int channelCount, int sampleRateHz = kDefaultSampleRateHz);

    // 通道名/单位（越界时返回 CH%1 与空单位）。
    QString channelName(int index) const;
    QString channelUnit(int index) const;

    // 是否有任何通道配置了非默认的增益/偏置。
    bool hasCalibration() const;

private:
    // 补齐 channels 到 channelCount，并做范围钳制。
    void normalize();
};

} // namespace aegis::core
