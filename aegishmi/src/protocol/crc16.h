#pragma once // 头文件保护：防止重复包含

#include <cstddef> // std::size_t
#include <cstdint> // std::uint8_t / std::uint16_t

namespace aegis::protocol {

// =============================================================================
// CRC16/MODBUS 校验
//
// CRC（循环冗余校验）是一种检错算法：发送方对一段字节计算 16 位校验值，
// 接收方用同样算法重算并比对，不一致就认为数据在传输中被破坏。
//
// 本实现使用 MODBUS 变体：多项式 0xA001（0x8005 的位反转形式），初值 0xFFFF。
// 它广泛用于工业设备通信（Modbus RTU 就使用它），面试中常被问到。
//
// 入参：data 指向待校验字节首地址，size 为字节数。
// 返回：16 位校验值。
// =============================================================================
std::uint16_t crc16(const std::uint8_t* data, std::size_t size);

} // namespace aegis::protocol
