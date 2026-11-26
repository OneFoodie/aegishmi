#include "protocol/modbus.h"

#include "protocol/crc16.h" // RTU 使用与自定义协议相同的 CRC16/MODBUS

namespace aegis::protocol::modbus {

namespace {

inline std::uint8_t highByte(std::uint16_t value)
{
    return static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

inline std::uint8_t lowByte(std::uint16_t value)
{
    return static_cast<std::uint8_t>(value & 0xFF);
}

inline std::uint16_t readU16Be(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

// 生成 PDU（功能码 + 数据），TCP 与 RTU 共用。
std::vector<std::uint8_t> buildPdu(const Request& request)
{
    std::vector<std::uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(static_cast<std::uint8_t>(request.function));
    pdu.push_back(highByte(request.startAddress));
    pdu.push_back(lowByte(request.startAddress));

    if (request.function == FunctionCode::WriteSingleRegister) {
        pdu.push_back(highByte(request.writeValue));
        pdu.push_back(lowByte(request.writeValue));
    } else {
        pdu.push_back(highByte(request.quantity));
        pdu.push_back(lowByte(request.quantity));
    }
    return pdu;
}

// 解析 PDU（从功能码开始）。size 为 PDU 可用字节数。
bool parsePdu(const std::uint8_t* pdu, std::size_t size, Response& out)
{
    if (size < 2)
        return false;

    const std::uint8_t function = pdu[0];

    // 异常响应：功能码最高位为 1，紧跟一个异常码。
    if ((function & 0x80) != 0) {
        out.isException = true;
        out.function = static_cast<FunctionCode>(function & 0x7F);
        out.exceptionCode = pdu[1];
        out.registers.clear();
        return true;
    }

    out.function = static_cast<FunctionCode>(function);
    out.isException = false;
    out.exceptionCode = 0;
    out.registers.clear();

    switch (out.function) {
    case FunctionCode::ReadHoldingRegisters:
    case FunctionCode::ReadInputRegisters: {
        // | FC | 字节数 | 数据... |
        if (size < 2)
            return false;
        const std::size_t byteCount = pdu[1];
        if (size < 2 + byteCount)
            return false;
        for (std::size_t i = 0; i + 1 < byteCount; i += 2)
            out.registers.push_back(readU16Be(pdu + 2 + i));
        return true;
    }
    case FunctionCode::WriteSingleRegister:
        // 写单个寄存器的响应是请求的回显：| FC | 地址 | 值 |
        return size >= 5;
    }
    return false;
}

} // namespace

// =============================================================================
// Modbus TCP
// =============================================================================
std::vector<std::uint8_t> encodeTcpRequest(std::uint16_t transactionId,
                                           const Request& request)
{
    const std::vector<std::uint8_t> pdu = buildPdu(request);

    std::vector<std::uint8_t> out;
    out.reserve(7 + pdu.size());
    out.push_back(highByte(transactionId)); // 事务号（大端）
    out.push_back(lowByte(transactionId));
    out.push_back(0); // 协议号固定 0
    out.push_back(0);
    const auto length = static_cast<std::uint16_t>(1 + pdu.size()); // 单元号 + PDU
    out.push_back(highByte(length));
    out.push_back(lowByte(length));
    out.push_back(request.unitId);
    out.insert(out.end(), pdu.begin(), pdu.end());
    return out;
}

std::size_t tcpFrameLength(const std::uint8_t* data, std::size_t available)
{
    if (data == nullptr || available < 6)
        return 0;

    const std::uint16_t length = readU16Be(data + 4);
    // 最小合法长度 = 单元号 1 + 功能码 1；最大值限制为 260（Modbus 规范上限附近）。
    if (length < 2 || length > 260)
        return 0;

    const std::size_t total = 6 + length;
    return available >= total ? total : 0;
}

bool decodeTcpResponse(const std::uint8_t* data, std::size_t size,
                       std::uint16_t expectedTransactionId, Response& out)
{
    if (data == nullptr || size < 9)
        return false;

    if (readU16Be(data) != expectedTransactionId)
        return false; // 事务号不匹配：丢弃迟到/串线的响应

    if (readU16Be(data + 2) != 0)
        return false; // 协议号必须为 0

    const std::uint16_t length = readU16Be(data + 4);
    if (length < 3 || size < static_cast<std::size_t>(6 + length))
        return false;

    out.unitId = data[6];
    // PDU 从偏移 7 开始，长度 = length - 1（去掉单元号）。
    return parsePdu(data + 7, static_cast<std::size_t>(length - 1), out);
}

// =============================================================================
// Modbus RTU
// =============================================================================
std::vector<std::uint8_t> encodeRtuRequest(const Request& request)
{
    const std::vector<std::uint8_t> pdu = buildPdu(request);

    std::vector<std::uint8_t> out;
    out.reserve(1 + pdu.size() + 2);
    out.push_back(request.unitId);
    out.insert(out.end(), pdu.begin(), pdu.end());

    const std::uint16_t crc = crc16(out.data(), out.size());
    out.push_back(lowByte(crc)); // RTU 的 CRC 低字节在前
    out.push_back(highByte(crc));
    return out;
}

std::size_t rtuFrameLength(const std::uint8_t* data, std::size_t available)
{
    if (data == nullptr || available < 2)
        return 0;

    const std::uint8_t function = data[1];

    // 异常响应：| 地址 | FC|0x80 | 异常码 | CRC | = 5 字节
    if ((function & 0x80) != 0)
        return available >= 5 ? 5 : 0;

    switch (static_cast<FunctionCode>(function)) {
    case FunctionCode::ReadHoldingRegisters:
    case FunctionCode::ReadInputRegisters: {
        // | 地址 | FC | 字节数 | 数据... | CRC |
        if (available < 3)
            return 0;
        const std::size_t total = 3 + data[2] + 2;
        return available >= total ? total : 0;
    }
    case FunctionCode::WriteSingleRegister:
        // | 地址 | FC | 地址 | 值 | CRC | = 8 字节
        return available >= 8 ? 8 : 0;
    }
    return 0;
}

bool decodeRtuResponse(const std::uint8_t* data, std::size_t size,
                       std::uint8_t expectedUnitId, Response& out)
{
    if (data == nullptr || size < 5)
        return false;
    if (data[0] != expectedUnitId)
        return false;

    // 校验 CRC：前 size-2 字节的 CRC 应等于末尾两个字节（低字节在前）。
    const std::uint16_t crcCalc = crc16(data, size - 2);
    const std::uint16_t crcRecv =
        static_cast<std::uint16_t>(data[size - 2] | (data[size - 1] << 8));
    if (crcCalc != crcRecv)
        return false;

    // PDU 从偏移 1 开始，去掉末尾 CRC。
    return parsePdu(data + 1, size - 3, out);
}

} // namespace aegis::protocol::modbus
