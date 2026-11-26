#include "protocol/crc16.h"

namespace aegis::protocol {

std::uint16_t crc16(const std::uint8_t* data, std::size_t size)
{
    // 初值固定为 0xFFFF，这是 MODBUS CRC 的规定。
    std::uint16_t crc = 0xFFFF;

    // 逐字节处理：先与当前字节异或，再逐位右移并条件异或多项式。
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];

        // 每个字节处理 8 位（一个字节的位宽）。
        for (int bit = 0; bit < 8; ++bit) {
            // 判断最低位是否为 1：
            //   是 -> 右移一位后与多项式 0xA001 异或（这是 CRC 的"模 2 除法"步骤）
            //   否 -> 直接右移一位
            if (crc & 0x0001)
                crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001);
            else
                crc = static_cast<std::uint16_t>(crc >> 1);
        }
    }

    return crc;
}

} // namespace aegis::protocol
