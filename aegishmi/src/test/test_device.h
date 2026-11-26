#pragma once // 头文件保护

#include "test/device_command.h"

#include <QDateTime>
#include <QString>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace aegis::test {

// =============================================================================
// ITestDevice —— 测试引擎依赖的设备抽象
//
// 为什么用抽象而不是直接调用 DeviceService？
//   - 单元测试可以注入 Mock，不需要真实设备/模拟器
//   - 以后换通信方式（TCP/串口/Modbus）不影响引擎
// =============================================================================

class ITestDevice {
public:
    virtual ~ITestDevice() = default;

    // 下发命令并等待应答。
    //   command/channel/value：命令内容
    //   timeoutMs：等待应答的超时
    //   status：返回设备应答的状态码（0 表示成功）
    //   linkId：目标链路（空表示主链路），多链路设备据此路由
    // 返回 false 表示超时、断线或发送失败。
    virtual bool sendCommand(DeviceCommand command, int channel, int value,
                             int timeoutMs, quint8& status,
                             const QString& linkId = QString()) = 0;

    // 读取设备文本信息（主要用于固件/软件版本号）。
    //   cmd：查询命令；为空时由实现方选择协议默认命令
    //        （SCPI 为 *IDN?，自定义协议为 READ_VERSION）
    //   value：成功时写入设备返回的原始文本（已去掉首尾空白）
    //   linkId：目标链路（空表示主链路）
    // 返回 false 表示不支持该协议、超时或设备返回错误。
    virtual bool queryText(const QString& cmd, int timeoutMs,
                           QString* value, QString* errorMessage,
                           const QString& linkId = QString()) = 0;

    // 采集指定通道在 windowMs 时间窗口内的所有采样值。
    // 实现方负责阻塞到窗口结束并返回数据；数据不足（掉线）时返回空或少量数据。
    //   linkId：目标链路（空表示主链路），多链路设备据此路由到对应链路采集
    virtual std::vector<std::int16_t> sampleWindow(int channel, int windowMs,
                                                   const QString& linkId = QString()) = 0;

    // 指定链路的实际采样率（Hz）。自定义协议为 10kHz，Modbus 轮询为 50Hz。
    // 引擎用它判断"采样窗口内应收到多少点"，避免用固定 10kHz 误判 Modbus 数据不足。
    virtual int sampleRateHz(const QString& linkId = QString()) const = 0;

    // 单点测量：等待一个采样周期后返回该通道的平均值（辅助链路常用）。
    // 默认实现基于 sampleWindow，各协议无需单独实现。
    //   timeoutMs：最长等待，<=0 时按一个采样周期计算
    // 窗口至少 20ms：照顾定时器抖动，避免 10kHz 链路在 10ms 窗口内一帧都没收到。
    virtual bool measureOnce(int channel, int timeoutMs, double* value,
                             QString* errorMessage, const QString& linkId = QString())
    {
        const int rate = sampleRateHz(linkId);
        int windowMs = std::max(20, 1000 / std::max(1, rate));
        if (timeoutMs > 0)
            windowMs = std::min(windowMs, timeoutMs);
        windowMs = std::max(10, windowMs);

        const std::vector<std::int16_t> samples = sampleWindow(channel, windowMs, linkId);
        if (samples.empty()) {
            if (errorMessage != nullptr)
                *errorMessage = QStringLiteral("单点测量未采集到数据");
            return false;
        }

        double sum = 0.0;
        for (const auto sample : samples)
            sum += sample;
        if (value != nullptr)
            *value = sum / static_cast<double>(samples.size());
        return true;
    }
};

// =============================================================================
// ITestClock —— 时间抽象
//
// 引擎里的 delay 与耗时统计都走这个接口。
// 测试时注入 MockClock 可以把 300ms 等待变成"瞬间完成 + 虚拟时间前进"，
// 既快又不会因为机器负载导致时序波动。
// =============================================================================
class ITestClock {
public:
    virtual ~ITestClock() = default;
    virtual void sleepMs(int ms) = 0;
    virtual qint64 nowMs() = 0;
};

// 生产环境使用的真实时钟。
class RealClock final : public ITestClock {
public:
    void sleepMs(int ms) override
    {
        if (ms > 0)
            QThread::msleep(static_cast<unsigned long>(ms));
    }

    qint64 nowMs() override { return QDateTime::currentMSecsSinceEpoch(); }
};

} // namespace aegis::test
