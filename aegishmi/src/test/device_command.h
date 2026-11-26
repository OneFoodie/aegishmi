#pragma once // 头文件保护

#include <QString>

namespace aegis::test {

// =============================================================================
// 设备命令字
//
// 流程配置里用字符串（SET_VOUT 等）描述命令，执行前转换成枚举。
// 真正的协议帧编解码在 M3.4 实现；这里只定义"引擎认识的命令"。
// =============================================================================
enum class DeviceCommand {
    SetVout,     // 设置输出电压（value 单位 mV）
    SetIout,     // 设置限流（value 单位 0.01A）
    ReadMeasure, // 读取指定通道瞬时值
    Reset,       // 复位设备
    Unknown,     // 配置里写了不认识的名字
};

inline DeviceCommand deviceCommandFromString(const QString& text)
{
    if (text == QLatin1String("SET_VOUT"))
        return DeviceCommand::SetVout;
    if (text == QLatin1String("SET_IOUT"))
        return DeviceCommand::SetIout;
    if (text == QLatin1String("READ_MEASURE"))
        return DeviceCommand::ReadMeasure;
    if (text == QLatin1String("RESET"))
        return DeviceCommand::Reset;
    return DeviceCommand::Unknown;
}

} // namespace aegis::test
