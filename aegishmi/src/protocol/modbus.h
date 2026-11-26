#pragma once // 头文件保护

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aegis::protocol::modbus {

// =============================================================================
// Modbus 协议编解码（TCP + RTU）
//
// 只实现工程需要的功能码：
//   0x03 读保持寄存器
//   0x04 读输入寄存器
//   0x06 写单个寄存器
//
// Modbus TCP 帧（MBAP + PDU）：
//   | 事务号 u16 BE | 协议号 u16 BE(=0) | 长度 u16 BE | 单元号 u8 | 功能码 u8 | 数据 |
//   长度字段 = 单元号(1) + PDU，不含前 6 字节。
//
// Modbus RTU 帧（ADU）：
//   | 从站地址 u8 | 功能码 u8 | 数据 | CRC16 u16（低字节在前）|
//   CRC 与自定义协议使用同一个 CRC16/MODBUS 算法。
//
// 大端（BE）：Modbus 规定多字节字段高位在前，注意与自定义协议的小端区分。
// =============================================================================

enum class FunctionCode : std::uint8_t {
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleRegister = 0x06,
};

// 一个请求。读操作使用 startAddress + quantity，写操作使用 startAddress + writeValue。
struct Request {
    std::uint8_t unitId = 1;
    FunctionCode function = FunctionCode::ReadInputRegisters;
    std::uint16_t startAddress = 0;
    std::uint16_t quantity = 1;
    std::uint16_t writeValue = 0;
};

// 一个响应（解析后的结果）。
struct Response {
    std::uint8_t unitId = 0;
    FunctionCode function = FunctionCode::ReadInputRegisters;
    bool isException = false;   // 设备返回异常响应（功能码最高位置 1）
    std::uint8_t exceptionCode = 0;
    std::vector<std::uint16_t> registers; // 读操作返回的寄存器；写操作为空
};

// ---- Modbus TCP ----

// 编码 TCP 请求。transactionId 用于请求/响应配对。
std::vector<std::uint8_t> encodeTcpRequest(std::uint16_t transactionId,
                                           const Request& request);

// 从缓冲区头部计算一整帧的字节数；数据不足或长度非法时返回 0。
std::size_t tcpFrameLength(const std::uint8_t* data, std::size_t available);

// 解析 TCP 响应（需传 complete frame）。事务号不匹配返回 false。
bool decodeTcpResponse(const std::uint8_t* data, std::size_t size,
                       std::uint16_t expectedTransactionId, Response& out);

// ---- Modbus RTU ----

// 编码 RTU 请求（自动追加 CRC）。
std::vector<std::uint8_t> encodeRtuRequest(const Request& request);

// 从缓冲区头部计算一整帧的字节数；数据不足或功能码未知时返回 0。
std::size_t rtuFrameLength(const std::uint8_t* data, std::size_t available);

// 解析 RTU 响应（校验 CRC、从站地址、异常码）。
bool decodeRtuResponse(const std::uint8_t* data, std::size_t size,
                       std::uint8_t expectedUnitId, Response& out);

} // namespace aegis::protocol::modbus
