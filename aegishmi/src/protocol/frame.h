#pragma once // 头文件保护：同一个编译单元内重复包含时只展开一次，防止重复定义

#include <array>   // std::array：固定长度数组，用于定长存储各通道采样值
#include <cstddef> // std::size_t：表示大小/下标的无符号整数类型
#include <cstdint> // std::uint8_t / std::uint16_t / std::int16_t 等定宽整数
#include <vector>  // std::vector：变长字节序列，用于帧负载

// 整个协议层放在 aegis::protocol 命名空间下，避免与其他库的同名符号冲突。
namespace aegis::protocol {

// =============================================================================
// 帧格式定义
//
//  | 0xAA 0x55 | len(u16 小端) | seq(u16 小端) | type(u8) | payload(len 字节) | crc16(u16 小端) |
//
//  - 帧头 0xAA 0x55：接收方靠它判断一帧从哪里开始（串口/TCP 都是字节流，没有天然边界）
//  - len：负载长度，接收方据此知道还要等多少字节
//  - seq：帧序号，用于发现丢帧、乱序
//  - type：帧类型（数据/心跳/应答等）
//  - crc16：校验码，用于发现传输过程中的比特错误
//
// CRC 覆盖范围是"len + seq + type + payload"，不包含帧头和 CRC 自身。
// 这是自定义协议的常见做法，真实项目里也可能是 Modbus、DL/T645 等标准协议。
// =============================================================================

// 帧头两个固定字节：接收方在字节流中搜索 0xAA 0x55 来对齐帧起始位置。
constexpr std::uint8_t kHeader0 = 0xAA;
constexpr std::uint8_t kHeader1 = 0x55;

// 一帧的固定开销 = 2 字节帧头 + 2 字节长度 + 2 字节序号 + 1 字节类型 + 2 字节 CRC = 9 字节。
// 实际帧长 = kFrameOverhead + payload 长度。
constexpr std::size_t kFrameOverhead = 9;

// 单个负载的最大长度。用于防御异常/恶意数据：长度字段若超过该值，说明帧头是误判。
// 中度规模（32~64 通道 × 100 点 ≈ 12.8 KB/帧）需要更大的上限。
constexpr std::size_t kMaxPayload = 65536;

// 默认通道数（设备配置未指定时使用）。实际通道数由 core::DeviceConfig 决定。
constexpr std::size_t kDefaultChannelCount = 6;

// 支持的最大通道数（配置上限，避免误配置把内存撑爆）。
constexpr std::size_t kMaxChannels = 64;

// 解码时用于推导"每帧采样点数"的默认采样率；实际采样率由设备配置给出。
constexpr int kSampleRateHz = 10000;

// 帧类型枚举。底层类型指定为 uint8_t，保证在协议中恰好占 1 字节。
enum class FrameType : std::uint8_t {
    Data = 0x01,    // 采样数据帧：负载是"时间戳 + 多通道采样值"
    Info = 0x02,    // 设备信息帧：负载可以是设备型号、固件版本等文本
    Ping = 0x10,    // 心跳帧：上位机定时发送，检查设备是否在线
    SetRate = 0x11, // 设置采样率帧（示例命令，当前模拟器未实现具体逻辑）
    Command = 0x20, // 命令帧：负载是"命令字 + 通道 + 参数"
    Ack = 0x81,     // 应答帧：设备对命令的回复
};

// 内存中的一帧（已解析完成，或待编码发送）。
struct Frame {
    std::uint16_t seq = 0;            // 序号；默认 0，发送方每发一帧自增
    FrameType type = FrameType::Info; // 类型；默认 Info，便于构造临时对象
    std::vector<std::uint8_t> payload; // 负载原始字节；具体含义由 type 决定
};

// DATA 类型帧的负载结构（解析后的结果，方便业务层直接使用）。
// 负载字节布局：u32 时间戳(ms，小端) + N 个 int16 采样值(小端，通道按序号交叠)。
// 例如 6 通道时，采样序列为：ch0 ch1 ch2 ch3 ch4 ch5 ch0 ch1 ...
struct DataPayload {
    std::uint32_t timestampMs = 0;       // 本批数据第一个采样点的时间戳（毫秒）
    std::vector<std::int16_t> samples;   // 交叠排列的采样值，长度 = 采样点数 × 通道数
};

// 把"时间戳 + 交叠采样数组"编码成 DATA 帧所需的字节序列（小端）。
// interleaved：指向采样值数组首元素；sampleCount：采样值总个数（不是采样点数）。
std::vector<std::uint8_t> encodeDataPayload(std::uint32_t timestampMs,
                                            const std::int16_t* interleaved,
                                            std::size_t sampleCount);

// 把 DATA 帧的负载字节解析回 DataPayload。
// 返回 false 表示负载格式非法（长度不足、字节数不是偶数等），调用方应丢弃该帧。
bool decodeDataPayload(const std::vector<std::uint8_t>& payload, DataPayload& out);

// =============================================================================
// 命令 / 应答（M3.4 新增，供自动测试流程使用）
//
// Command 帧负载（4 字节）：cmdId(u8) | channel(u8) | value(i16 LE) | 保留(u8)
// Ack     帧负载（4 字节）：cmdId(u8) | status(u8)  | measured(i16 LE) | 保留(u8)
//
// status：0 = 成功，1 = 参数越界，2 = 设备忙，3 = 不支持的命令
// =============================================================================

// 命令字。与上位机配置里的字符串（SET_VOUT 等）一一对应。
enum class CommandId : std::uint8_t {
    SetVout = 0x01,     // 设置输出电压，value 单位 mV
    SetIout = 0x02,     // 设置输出电流，value 单位 mA
    ReadMeasure = 0x03, // 读取指定通道瞬时值
    Reset = 0x04,       // 恢复默认设定
    ReadVersion = 0x05, // 读取固件/软件版本，设备以 Info 帧回复文本
};

// 命令负载（解析后的结构）。
struct CommandPayload {
    CommandId command = CommandId::Reset;
    std::uint8_t channel = 0; // 仅 READ_MEASURE 使用
    std::int16_t value = 0;   // 命令参数（含义由命令字决定）
};

// 应答负载（解析后的结构）。
struct AckPayload {
    std::uint8_t command = 0;  // 回显的命令字
    std::uint8_t status = 0;   // 0 成功；非 0 见上方错误码
    std::int16_t measured = 0; // READ_MEASURE 时返回的瞬时值
};

std::vector<std::uint8_t> encodeCommandPayload(const CommandPayload& payload);
std::vector<std::uint8_t> encodeAckPayload(const AckPayload& payload);
bool decodeCommandPayload(const std::vector<std::uint8_t>& payload, CommandPayload& out);
bool decodeAckPayload(const std::vector<std::uint8_t>& payload, AckPayload& out);

} // namespace aegis::protocol
