// Qt Test 框架：提供 QCOMPARE / QVERIFY 断言、测试运行器与 main 宏
#include <QtTest>

#include "core/chunk_ring_buffer.h"
#include "core/spsc_ring_buffer.h"
#include "protocol/crc16.h"
#include "protocol/frame_codec.h"

using namespace aegis;

// =============================================================================
// TstCore —— 核心模块单元测试
//
// 覆盖点：
//   - CRC16 与标准样例一致（防止算法写错）
//   - DATA 负载编码/解码可逆
//   - 整帧编码/解码可逆
//   - 分包（一字节一字节到达）能正确还原
//   - 粘包（两帧一次到达）能正确拆开
//   - CRC 被破坏时丢弃该帧并计数
//   - 环形缓冲先进先出、写满后拒绝写入
//
// 每个 private slots 函数都是一个独立测试用例，由 QTest 自动执行。
// =============================================================================
class TstCore : public QObject {
    Q_OBJECT

private slots:
    void crc16KnownVector();
    void dataPayloadRoundtrip();
    void codecRoundtrip();
    void codecHandlesFragments();
    void codecHandlesCoalescedFrames();
    void codecDetectsCorruptedFrame();
    void ringBufferFifo();
    void ringBufferOverflow();
    void commandPayloadRoundtrip();
    void ackPayloadRoundtrip();
    void commandPayloadTooShort();
    void commandFrameThroughCodec();
    void chunkRingFifo();
    void chunkRingOverflowAndClear();
};

// 标准校验样例："123456789" 的 CRC16/MODBUS 结果固定为 0x4B37。
// 若算法参数（多项式/初值）写错，这里会立刻失败。
void TstCore::crc16KnownVector()
{
    const QByteArray text("123456789");
    const auto value = protocol::crc16(
        reinterpret_cast<const std::uint8_t*>(text.constData()),
        static_cast<std::size_t>(text.size()));
    QCOMPARE(static_cast<int>(value), 0x4B37);
}

// 编码后再解码，时间戳与 6 个采样值应完全一致。
void TstCore::dataPayloadRoundtrip()
{
    const std::int16_t samples[] = {1, -2, 3, -4, 5, -6};
    const auto payload = protocol::encodeDataPayload(123456u, samples, 6);

    protocol::DataPayload decoded;
    QVERIFY(protocol::decodeDataPayload(payload, decoded)); // 解析必须成功
    QCOMPARE(decoded.timestampMs, 123456u);
    QCOMPARE(decoded.samples.size(), std::size_t(6));
    for (std::size_t i = 0; i < 6; ++i)
        QCOMPARE(static_cast<int>(decoded.samples[i]), static_cast<int>(samples[i]));
}

// 整帧编解码：序号、类型、负载都应保持不变，且无残留字节、无 CRC 错误。
void TstCore::codecRoundtrip()
{
    protocol::Frame frame;
    frame.seq = 42;
    frame.type = protocol::FrameType::Data;
    frame.payload = {1, 2, 3, 4, 5};

    const auto bytes = protocol::FrameCodec::encode(frame);

    protocol::FrameCodec codec;
    int count = 0; // 回调被调用的次数
    codec.feed(bytes.data(), bytes.size(), [&](const protocol::Frame& decoded) {
        ++count;
        QCOMPARE(decoded.seq, frame.seq);
        QCOMPARE(static_cast<std::uint8_t>(decoded.type),
                 static_cast<std::uint8_t>(frame.type));
        QVERIFY(decoded.payload == frame.payload);
    });

    QCOMPARE(count, 1);
    QCOMPARE(static_cast<int>(codec.framesDecoded()), 1);
    QCOMPARE(static_cast<int>(codec.crcErrors()), 0);
    QCOMPARE(static_cast<int>(codec.pendingBytes()), 0); // 全部消费完，没有残留
}

// 分包场景：把两帧拼成的字节流逐字节灌入，验证解码器能等数据齐了再解析。
// 这是串口通信最典型的情况（一次 read 只收到 1 个字节）。
void TstCore::codecHandlesFragments()
{
    protocol::Frame first;
    first.seq = 1;
    first.type = protocol::FrameType::Data;
    first.payload = {10, 20, 30};

    protocol::Frame second;
    second.seq = 2;
    second.type = protocol::FrameType::Ack;
    second.payload = {0x55, 0xAA}; // 故意让负载里出现帧头字节，验证不会误判

    auto stream = protocol::FrameCodec::encode(first);
    const auto secondBytes = protocol::FrameCodec::encode(second);
    stream.insert(stream.end(), secondBytes.begin(), secondBytes.end());

    protocol::FrameCodec codec;
    std::vector<std::uint16_t> seqs;
    for (std::uint8_t byte : stream) {
        codec.feed(&byte, 1, [&](const protocol::Frame& decoded) {
            seqs.push_back(decoded.seq);
        });
    }

    QCOMPARE(seqs.size(), std::size_t(2));
    QCOMPARE(seqs[0], std::uint16_t(1));
    QCOMPARE(seqs[1], std::uint16_t(2));
}

// 粘包场景：两帧一次性到达，解码器应在一次 feed 中回调两次。
void TstCore::codecHandlesCoalescedFrames()
{
    protocol::Frame frame;
    frame.seq = 7;
    frame.type = protocol::FrameType::Data;
    frame.payload = {1, 2, 3};

    auto stream = protocol::FrameCodec::encode(frame);
    const auto duplicated = protocol::FrameCodec::encode(frame);
    stream.insert(stream.end(), duplicated.begin(), duplicated.end());

    protocol::FrameCodec codec;
    int count = 0;
    codec.feed(stream.data(), stream.size(), [&](const protocol::Frame&) { ++count; });
    QCOMPARE(count, 2);
}

// 校验错误场景：破坏 CRC 后，该帧不能被交给上层，同时错误计数 +1。
void TstCore::codecDetectsCorruptedFrame()
{
    protocol::Frame frame;
    frame.seq = 9;
    frame.type = protocol::FrameType::Data;
    frame.payload = {7, 7, 7, 7};

    auto bytes = protocol::FrameCodec::encode(frame);
    bytes.back() ^= 0xFF; // 翻转 CRC 最后一个字节

    protocol::FrameCodec codec;
    int count = 0;
    codec.feed(bytes.data(), bytes.size(), [&](const protocol::Frame&) { ++count; });

    QCOMPARE(count, 0);
    QVERIFY(codec.crcErrors() >= 1);
}

// 环形缓冲基本行为：先进先出，pop 后 size 正确减少。
void TstCore::ringBufferFifo()
{
    core::SpscRingBuffer<int, 8> ring;
    for (int i = 0; i < 5; ++i)
        QVERIFY(ring.push(i));

    QCOMPARE(static_cast<int>(ring.size()), 5);

    int out[8] = {};
    QCOMPARE(static_cast<int>(ring.pop(out, 3)), 3);
    QCOMPARE(out[0], 0);
    QCOMPARE(out[1], 1);
    QCOMPARE(out[2], 2);
    QCOMPARE(static_cast<int>(ring.size()), 2);
}

// 环形缓冲溢出行为：容量 8 时只能写入 8 个，超出部分返回 false（由上层计丢块）。
void TstCore::ringBufferOverflow()
{
    core::SpscRingBuffer<int, 8> ring;
    int accepted = 0;
    for (int i = 0; i < 20; ++i) {
        if (ring.push(i))
            ++accepted;
    }

    QCOMPARE(accepted, 8);
    QCOMPARE(static_cast<int>(ring.size()), 8);

    int out[8] = {};
    QCOMPARE(static_cast<int>(ring.pop(out, 8)), 8);
    QCOMPARE(out[0], 0); // 最旧的仍在队首
    QCOMPARE(out[7], 7); // 最新的在队尾
}

// 命令负载编解码：有符号参数也要能原样往返。
void TstCore::commandPayloadRoundtrip()
{
    protocol::CommandPayload input;
    input.command = protocol::CommandId::SetVout;
    input.channel = 2;
    input.value = -1234; // 负数验证符号处理

    const auto bytes = protocol::encodeCommandPayload(input);
    QCOMPARE(bytes.size(), std::size_t(4));

    protocol::CommandPayload output;
    QVERIFY(protocol::decodeCommandPayload(bytes, output));
    QCOMPARE(static_cast<int>(output.command), static_cast<int>(input.command));
    QCOMPARE(static_cast<int>(output.channel), static_cast<int>(input.channel));
    QCOMPARE(static_cast<int>(output.value), static_cast<int>(input.value));
}

// 应答负载编解码。
void TstCore::ackPayloadRoundtrip()
{
    protocol::AckPayload input;
    input.command = 0x03;
    input.status = 2;
    input.measured = -300;

    const auto bytes = protocol::encodeAckPayload(input);
    QCOMPARE(bytes.size(), std::size_t(4));

    protocol::AckPayload output;
    QVERIFY(protocol::decodeAckPayload(bytes, output));
    QCOMPARE(static_cast<int>(output.command), static_cast<int>(input.command));
    QCOMPARE(static_cast<int>(output.status), static_cast<int>(input.status));
    QCOMPARE(static_cast<int>(output.measured), static_cast<int>(input.measured));
}

// 负载长度不足时必须拒绝，不能读越界。
void TstCore::commandPayloadTooShort()
{
    const std::vector<std::uint8_t> payload = {0x01, 0x02};
    protocol::CommandPayload command;
    QVERIFY(!protocol::decodeCommandPayload(payload, command));
    protocol::AckPayload ack;
    QVERIFY(!protocol::decodeAckPayload(payload, ack));
}

// 命令帧走完整链路：编码 -> 流式解码 -> 解析负载，类型/序号/参数都要正确。
void TstCore::commandFrameThroughCodec()
{
    protocol::CommandPayload command;
    command.command = protocol::CommandId::SetIout;
    command.channel = 1;
    command.value = 2500;

    protocol::Frame frame;
    frame.type = protocol::FrameType::Command;
    frame.seq = 33;
    frame.payload = protocol::encodeCommandPayload(command);

    const auto bytes = protocol::FrameCodec::encode(frame);

    protocol::FrameCodec codec;
    int count = 0;
    protocol::CommandPayload decoded;
    codec.feed(bytes.data(), bytes.size(), [&](const protocol::Frame& received) {
        ++count;
        QCOMPARE(static_cast<int>(received.type),
                 static_cast<int>(protocol::FrameType::Command));
        QCOMPARE(received.seq, std::uint16_t(33));
        QVERIFY(protocol::decodeCommandPayload(received.payload, decoded));
    });

    QCOMPARE(count, 1);
    QCOMPARE(static_cast<int>(decoded.command),
             static_cast<int>(protocol::CommandId::SetIout));
    QCOMPARE(static_cast<int>(decoded.channel), 1);
    QCOMPARE(static_cast<int>(decoded.value), 2500);
}

// =============================================================================
// ChunkRingBuffer：按块存储（多通道 × 多点）的先进先出与满/清空行为
// =============================================================================
void TstCore::chunkRingFifo()
{
    core::ChunkRingBuffer ring(4, 8, 4); // 4 通道，单块最多 8 点，4 个槽位
    QCOMPARE(ring.channelCount(), 4);

    // 两块数据：第一块 2 点，第二块 3 点。
    const std::int16_t first[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::int16_t second[12] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    QVERIFY(ring.push(100, 0.1f, first, 2));
    QVERIFY(ring.push(200, 0.2f, second, 3));
    QCOMPARE(ring.size(), std::size_t(2));

    int index = 0;
    ring.consume([&](const core::ChunkView& view) {
        if (index == 0) {
            QCOMPARE(view.timestampMs, std::uint32_t(100));
            QCOMPARE(view.pointCount, 2);
            QCOMPARE(view.channelCount, 4);
            QCOMPARE(view.samples[0], std::int16_t(1));
            QCOMPARE(view.samples[7], std::int16_t(8));
        } else {
            QCOMPARE(view.timestampMs, std::uint32_t(200));
            QCOMPARE(view.pointCount, 3);
            QCOMPARE(view.samples[0], std::int16_t(11));
            QCOMPARE(view.samples[11], std::int16_t(22));
        }
        ++index;
    });
    QCOMPARE(index, 2);
    QCOMPARE(ring.size(), std::size_t(0));
}

void TstCore::chunkRingOverflowAndClear()
{
    core::ChunkRingBuffer ring(2, 4, 2); // 2 槽位
    const std::int16_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    QVERIFY(ring.push(1, 0.0f, data, 4));
    QVERIFY(ring.push(2, 0.0f, data, 4));
    // 满：第 3 次应失败，且不会破坏已入队数据。
    QVERIFY(!ring.push(3, 0.0f, data, 4));
    QCOMPARE(ring.size(), std::size_t(2));

    // 非法参数：点数超过单块上限。
    QVERIFY(!ring.push(4, 0.0f, data, 5));

    ring.clear();
    QCOMPARE(ring.size(), std::size_t(0));
    QVERIFY(ring.push(5, 0.0f, data, 4)); // 清空后可继续使用
}

// QTEST_GUILESS_MAIN：生成 main()，不创建 QApplication（纯逻辑测试不需要 GUI）。
QTEST_GUILESS_MAIN(TstCore)

// Q_OBJECT 在 .cpp 中需要包含 moc 输出。
#include "tst_core.moc"
