// Qt Test 框架
#include <QtTest>

#include "protocol/crc16.h"
#include "protocol/modbus.h"

using namespace aegis::protocol;
using namespace aegis::protocol::modbus;

namespace {

void appendU16Be(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

// 构造一个 RTU 响应：地址 + FC04 + 字节数 + 寄存器 + CRC
std::vector<std::uint8_t> makeRtuReadResponse(std::uint8_t unitId,
                                              const std::vector<std::uint16_t>& registers)
{
    std::vector<std::uint8_t> out;
    out.push_back(unitId);
    out.push_back(0x04);
    out.push_back(static_cast<std::uint8_t>(registers.size() * 2));
    for (const auto value : registers)
        appendU16Be(out, value);

    const std::uint16_t crc = crc16(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
    return out;
}

// 构造一个 TCP 响应（MBAP + PDU）
std::vector<std::uint8_t> makeTcpReadResponse(std::uint16_t transactionId,
                                              std::uint8_t unitId,
                                              const std::vector<std::uint16_t>& registers)
{
    std::vector<std::uint8_t> out;
    appendU16Be(out, transactionId);
    appendU16Be(out, 0); // 协议号
    appendU16Be(out, static_cast<std::uint16_t>(3 + registers.size() * 2)); // 单元号+FC+字节数+数据
    out.push_back(unitId);
    out.push_back(0x04);
    out.push_back(static_cast<std::uint8_t>(registers.size() * 2));
    for (const auto value : registers)
        appendU16Be(out, value);
    return out;
}

} // namespace

// =============================================================================
// TstModbus —— Modbus 编解码
// =============================================================================
class TstModbus : public QObject {
    Q_OBJECT

private slots:
    void tcpReadRequestExactBytes();
    void tcpHoldingRegistersRequestExactBytes();
    void tcpWriteRequestExactBytes();
    void tcpResponseParsesRegisters();
    void tcpExceptionResponse();
    void tcpFrameLengthHandlesPartial();
    void rtuRequestHasValidCrc();
    void rtuResponseParsesRegisters();
    void rtuRejectsBadCrc();
    void rtuFrameLengthHandlesPartial();
};

void TstModbus::tcpReadRequestExactBytes()
{
    Request request;
    request.unitId = 1;
    request.function = FunctionCode::ReadInputRegisters;
    request.startAddress = 0;
    request.quantity = 6;

    const auto bytes = encodeTcpRequest(1, request);

    const std::vector<std::uint8_t> expected = {
        0x00, 0x01,             // 事务号
        0x00, 0x00,             // 协议号
        0x00, 0x06,             // 长度 = 单元号 + PDU
        0x01,                   // 单元号
        0x04,                   // 功能码
        0x00, 0x00,             // 起始地址
        0x00, 0x06              // 数量
    };
    QVERIFY(bytes == expected);
}

void TstModbus::tcpHoldingRegistersRequestExactBytes()
{
    // 版本读取使用 FC03 读 0x0100 起 12 个保持寄存器。
    Request request;
    request.unitId = 1;
    request.function = FunctionCode::ReadHoldingRegisters;
    request.startAddress = 0x0100;
    request.quantity = 12;

    const auto bytes = encodeTcpRequest(3, request);

    const std::vector<std::uint8_t> expected = {
        0x00, 0x03, // 事务号
        0x00, 0x00, // 协议号
        0x00, 0x06, // 长度
        0x01,       // 单元号
        0x03,       // 功能码：读保持寄存器
        0x01, 0x00, // 起始地址 0x0100
        0x00, 0x0C  // 数量 12
    };
    QVERIFY(bytes == expected);
}

void TstModbus::tcpWriteRequestExactBytes()
{
    Request request;
    request.unitId = 1;
    request.function = FunctionCode::WriteSingleRegister;
    request.startAddress = 0;
    request.writeValue = 6000; // 0x1770

    const auto bytes = encodeTcpRequest(2, request);

    const std::vector<std::uint8_t> expected = {
        0x00, 0x02,
        0x00, 0x00,
        0x00, 0x06,
        0x01,
        0x06,
        0x00, 0x00,
        0x17, 0x70
    };
    QVERIFY(bytes == expected);
}

void TstModbus::tcpResponseParsesRegisters()
{
    const auto frame = makeTcpReadResponse(7, 1, {5000, 1000, 250, 0, 0, 0});
    QCOMPARE(tcpFrameLength(frame.data(), frame.size()), frame.size());

    Response response;
    QVERIFY(decodeTcpResponse(frame.data(), frame.size(), 7, response));
    QVERIFY(!response.isException);
    QCOMPARE(static_cast<int>(response.unitId), 1);
    QCOMPARE(response.registers.size(), std::size_t(6));
    QCOMPARE(static_cast<int>(response.registers[0]), 5000);
    QCOMPARE(static_cast<int>(response.registers[1]), 1000);
    QCOMPARE(static_cast<int>(response.registers[2]), 250);

    // 事务号不匹配必须拒绝
    QVERIFY(!decodeTcpResponse(frame.data(), frame.size(), 8, response));
}

void TstModbus::tcpExceptionResponse()
{
    std::vector<std::uint8_t> frame;
    appendU16Be(frame, 1);
    appendU16Be(frame, 0);
    appendU16Be(frame, 3); // 单元号 + FC + 异常码
    frame.push_back(1);
    frame.push_back(0x84); // 0x04 | 0x80
    frame.push_back(2);    // 非法数据地址

    Response response;
    QVERIFY(decodeTcpResponse(frame.data(), frame.size(), 1, response));
    QVERIFY(response.isException);
    QCOMPARE(static_cast<int>(response.exceptionCode), 2);
}

void TstModbus::tcpFrameLengthHandlesPartial()
{
    const auto frame = makeTcpReadResponse(1, 1, {1, 2});

    QCOMPARE(tcpFrameLength(frame.data(), 4), std::size_t(0)); // 头部都不完整
    QCOMPARE(tcpFrameLength(frame.data(), frame.size() - 1), std::size_t(0)); // 半包
    QCOMPARE(tcpFrameLength(frame.data(), frame.size()), frame.size());
}

void TstModbus::rtuRequestHasValidCrc()
{
    Request request;
    request.unitId = 1;
    request.function = FunctionCode::ReadInputRegisters;
    request.startAddress = 0;
    request.quantity = 6;

    const auto bytes = encodeRtuRequest(request);

    // 地址 + FC + 地址(2) + 数量(2) + CRC(2) = 8 字节
    QCOMPARE(bytes.size(), std::size_t(8));
    QCOMPARE(static_cast<int>(bytes[0]), 1);
    QCOMPARE(static_cast<int>(bytes[1]), 4);

    const std::uint16_t crc = crc16(bytes.data(), 6);
    QCOMPARE(static_cast<int>(bytes[6]), static_cast<int>(crc & 0xFF));
    QCOMPARE(static_cast<int>(bytes[7]), static_cast<int>((crc >> 8) & 0xFF));
}

void TstModbus::rtuResponseParsesRegisters()
{
    const auto frame = makeRtuReadResponse(1, {5000, 1000, 250});

    QCOMPARE(rtuFrameLength(frame.data(), frame.size()), frame.size());
    QCOMPARE(rtuFrameLength(frame.data(), frame.size() - 1), std::size_t(0));

    Response response;
    QVERIFY(decodeRtuResponse(frame.data(), frame.size(), 1, response));
    QVERIFY(!response.isException);
    QCOMPARE(response.registers.size(), std::size_t(3));
    QCOMPARE(static_cast<int>(response.registers[0]), 5000);

    // 从站地址不匹配必须拒绝
    QVERIFY(!decodeRtuResponse(frame.data(), frame.size(), 2, response));
}

void TstModbus::rtuRejectsBadCrc()
{
    auto frame = makeRtuReadResponse(1, {1234, 5678});
    frame.back() ^= 0xFF; // 破坏 CRC

    Response response;
    QVERIFY(!decodeRtuResponse(frame.data(), frame.size(), 1, response));
}

void TstModbus::rtuFrameLengthHandlesPartial()
{
    const auto frame = makeRtuReadResponse(1, {1, 2, 3});

    QCOMPARE(rtuFrameLength(frame.data(), 1), std::size_t(0));    // 连功能码都没有
    QCOMPARE(rtuFrameLength(frame.data(), frame.size() - 2), std::size_t(0)); // 缺 CRC
    QCOMPARE(rtuFrameLength(frame.data(), frame.size()), frame.size());
}

QTEST_GUILESS_MAIN(TstModbus)

#include "tst_modbus.moc"
