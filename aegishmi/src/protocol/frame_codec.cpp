#include "protocol/frame_codec.h"
#include "protocol/crc16.h" // crc16()

#include <algorithm> // std::min

namespace aegis::protocol {

namespace {
// 一帧至少有 2(帧头) + 2(长度) + 2(序号) + 1(类型) + 2(CRC) = 9 字节（负载为 0 时）。
// 缓冲区剩余不足 9 字节时，连长度字段都读不全，必须先等更多数据。
constexpr std::size_t kMinFrameSize = kFrameOverhead;
} // namespace

// =============================================================================
// encode：内存 Frame -> 字节流
// 顺序严格按协议：AA 55 | len | seq | type | payload | crc
// 所有多字节整数都按"小端"写入（低字节在前），这是工业设备与 x86 上最常见的约定。
// =============================================================================
std::vector<std::uint8_t> FrameCodec::encode(const Frame& frame)
{
    const std::size_t len = frame.payload.size();

    // 预先分配容量：负载长度 + 固定开销。避免 push_back 过程中反复扩容。
    std::vector<std::uint8_t> out;
    out.reserve(len + kFrameOverhead);

    out.push_back(kHeader0); // 帧头第 1 字节 0xAA
    out.push_back(kHeader1); // 帧头第 2 字节 0x55

    // 长度字段：u16 小端 —— 低 8 位先写，高 8 位后写。
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));

    // 序号字段：u16 小端。
    out.push_back(static_cast<std::uint8_t>(frame.seq & 0xFF));
    out.push_back(static_cast<std::uint8_t>((frame.seq >> 8) & 0xFF));

    // 类型字段：1 字节。
    out.push_back(static_cast<std::uint8_t>(frame.type));

    // 负载原样追加（0 字节负载也允许，例如心跳帧）。
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());

    // CRC 覆盖 "len + seq + type + payload"：
    //   out.data() + 2 跳过 2 字节帧头；out.size() - 2 表示到负载末尾（此时还没写 CRC）。
    const std::uint16_t crc = crc16(out.data() + 2, out.size() - 2);
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));        // CRC 低字节
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF)); // CRC 高字节

    return out;
}

// =============================================================================
// feed：字节流 -> 一帧或多帧
//
// 核心循环：
//   1. 在缓冲区里找帧头 0xAA 0x55
//   2. 读长度字段，判断这一帧总共多少字节
//   3. 数据不够 -> 退出循环，等下次 feed 再来
//   4. 数据够了 -> 校验 CRC
//        正确 -> 构造 Frame 回调给上层，pos 跳过整帧
//        错误 -> pos 只前进 1 字节，重新找帧头（可能只是帧头误判）
//   5. 循环结束后，把 pos 之前已消费的字节从缓冲区删除
// =============================================================================
void FrameCodec::feed(const std::uint8_t* data, std::size_t size, const FrameHandler& handler)
{
    // 第一步：把新到的字节追加到缓冲区尾部。data 允许为空（仅处理已有残留数据）。
    if (data != nullptr && size > 0)
        buffer_.insert(buffer_.end(), data, data + size);

    // pos 表示"当前正在检查的起始下标"，它之前的字节都已经被处理/丢弃。
    std::size_t pos = 0;

    // 条件 pos + 2 <= size：至少要有 2 个字节才可能构成帧头，否则直接结束。
    while (pos + 2 <= buffer_.size()) {
        // ---------- 1) 对齐帧头 ----------
        // 如果当前位置不是 0xAA 0x55，就往后挪一个字节继续找。
        // 这相当于在字节流中"扫描"帧起始位置，能跳过断线/干扰产生的垃圾字节。
        if (buffer_[pos] != kHeader0 || buffer_[pos + 1] != kHeader1) {
            ++pos;
            continue;
        }

        // ---------- 2) 头部完整性检查 ----------
        // 剩余不足 9 字节时，长度字段或类型字段都读不全，无法判断帧长。
        // 注意：这里用 break 而不是 continue，因为要保留数据等下一次 feed。
        if (buffer_.size() - pos < kMinFrameSize)
            break;

        // 读取长度字段（小端）：低字节在 pos+2，高字节在 pos+3。
        const std::uint16_t len =
            static_cast<std::uint16_t>(buffer_[pos + 2] | (buffer_[pos + 3] << 8));

        // ---------- 3) 长度合法性检查 ----------
        // 长度超过协议上限，说明这个 0xAA 0x55 只是数据里恰好出现的巧合，
        // 不是真正的帧头。跳过一个字节，继续重新找。
        if (len > kMaxPayload) {
            ++pos;
            continue;
        }

        // 这一帧的总长度 = 固定开销 + 负载长度。
        const std::size_t total = kFrameOverhead + len;

        // ---------- 4) 整帧是否到齐 ----------
        // 剩余字节不足一整帧：半包，退出循环等待后续数据。
        if (buffer_.size() - pos < total)
            break;

        // 指向当前候选帧的起始地址，后面按偏移读取各字段。
        const std::uint8_t* framePtr = buffer_.data() + pos;

        // ---------- 5) CRC 校验 ----------
        // 计算范围：跳过 2 字节帧头，长度 = 整帧长度 - 2(帧头) - 2(CRC)。
        const std::uint16_t crcCalc = crc16(framePtr + 2, total - 4);
        // 接收到的 CRC：整帧最后 2 字节（小端）。
        const std::uint16_t crcRecv = static_cast<std::uint16_t>(
            framePtr[total - 2] | (framePtr[total - 1] << 8));

        if (crcCalc != crcRecv) {
            // 校验失败：可能是传输误码，也可能是帧头误判。
            // 只前进 1 字节重新同步，避免整段数据被误丢弃。
            ++crcErrors_;
            ++pos;
            continue;
        }

        // ---------- 6) 解析字段并回调 ----------
        Frame frame;
        // 序号位于帧头 + 长度之后，即偏移 4、5（小端）。
        frame.seq = static_cast<std::uint16_t>(framePtr[4] | (framePtr[5] << 8));
        // 类型位于偏移 6。
        frame.type = static_cast<FrameType>(framePtr[6]);
        // 负载从偏移 7 开始，长度为 len。
        frame.payload.assign(framePtr + 7, framePtr + 7 + len);

        ++framesDecoded_;
        // 把解析结果交给上层。注意：handler 里不要做耗时操作，否则会阻塞收包线程。
        if (handler)
            handler(frame);

        // 跳过整帧，继续解析缓冲区里可能紧跟着的下一帧（粘包场景）。
        pos += total;
    }

    // 循环结束后，删除 [0, pos) 这段已处理完的字节，避免缓冲区无限增长。
    // 剩下的字节就是"还没凑齐的半帧"或"待重新对齐的垃圾数据"。
    if (pos > 0)
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
}

void FrameCodec::reset()
{
    buffer_.clear();     // 丢弃残留字节
    framesDecoded_ = 0;  // 统计清零
    crcErrors_ = 0;
}

// =============================================================================
// encodeDataPayload：把采样数据打包成 DATA 帧的负载
//
// 布局：[时间戳 u32 小端][采样0 int16 小端][采样1 int16 小端]...
// =============================================================================
std::vector<std::uint8_t> encodeDataPayload(std::uint32_t timestampMs,
                                            const std::int16_t* interleaved,
                                            std::size_t sampleCount)
{
    // 4 字节时间戳 + 每个采样 2 字节。
    std::vector<std::uint8_t> out(4 + sampleCount * 2, 0);

    // 时间戳按小端拆成 4 个字节。
    out[0] = static_cast<std::uint8_t>(timestampMs & 0xFF);
    out[1] = static_cast<std::uint8_t>((timestampMs >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((timestampMs >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((timestampMs >> 24) & 0xFF);

    // 每个 int16 采样拆成低字节、高字节依次写入。
    // 先转成 uint16_t 是为了避免有符号数右移带来的符号位扩展问题。
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const auto v = static_cast<std::uint16_t>(interleaved[i]);
        out[4 + i * 2] = static_cast<std::uint8_t>(v & 0xFF);
        out[5 + i * 2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    }
    return out;
}

// =============================================================================
// decodeDataPayload：DATA 帧负载 -> 时间戳 + 采样数组
// 返回 false 表示负载非法，上层应忽略该帧（不能让坏数据进入显示/存储逻辑）。
// =============================================================================
bool decodeDataPayload(const std::vector<std::uint8_t>& payload, DataPayload& out)
{
    // 至少要能放下 4 字节时间戳。
    if (payload.size() < 4)
        return false;

    // 去掉时间戳后，剩余字节数必须是 2 的整数倍（int16 采样）。
    const std::size_t bytes = payload.size() - 4;
    if (bytes % 2 != 0)
        return false;

    // 还原小端时间戳。
    out.timestampMs = static_cast<std::uint32_t>(payload[0]) |
                      (static_cast<std::uint32_t>(payload[1]) << 8) |
                      (static_cast<std::uint32_t>(payload[2]) << 16) |
                      (static_cast<std::uint32_t>(payload[3]) << 24);

    // 还原每个 int16：先拼成 uint16_t，再按位转成有符号 int16_t。
    const std::size_t count = bytes / 2;
    out.samples.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.samples[i] = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(payload[4 + i * 2] | (payload[5 + i * 2] << 8)));
    }
    return true;
}

// =============================================================================
// 命令 / 应答负载编解码（固定 4 字节，小端）
// =============================================================================
std::vector<std::uint8_t> encodeCommandPayload(const CommandPayload& payload)
{
    std::vector<std::uint8_t> out(4, 0);
    out[0] = static_cast<std::uint8_t>(payload.command);
    out[1] = payload.channel;
    // value 是有符号 16 位，先转成无符号再拆字节，避免右移时的符号扩展。
    const auto value = static_cast<std::uint16_t>(payload.value);
    out[2] = static_cast<std::uint8_t>(value & 0xFF);
    out[3] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    return out;
}

std::vector<std::uint8_t> encodeAckPayload(const AckPayload& payload)
{
    std::vector<std::uint8_t> out(4, 0);
    out[0] = payload.command;
    out[1] = payload.status;
    const auto measured = static_cast<std::uint16_t>(payload.measured);
    out[2] = static_cast<std::uint8_t>(measured & 0xFF);
    out[3] = static_cast<std::uint8_t>((measured >> 8) & 0xFF);
    return out;
}

bool decodeCommandPayload(const std::vector<std::uint8_t>& payload, CommandPayload& out)
{
    if (payload.size() < 4)
        return false;
    out.command = static_cast<CommandId>(payload[0]);
    out.channel = payload[1];
    out.value = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(payload[2] | (payload[3] << 8)));
    return true;
}

bool decodeAckPayload(const std::vector<std::uint8_t>& payload, AckPayload& out)
{
    if (payload.size() < 4)
        return false;
    out.command = payload[0];
    out.status = payload[1];
    out.measured = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(payload[2] | (payload[3] << 8)));
    return true;
}

} // namespace aegis::protocol
