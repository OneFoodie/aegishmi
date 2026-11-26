#pragma once // 头文件保护

#include "protocol/frame.h" // 需要 Frame、FrameType、kMaxPayload 等定义

#include <functional> // std::function：用回调函数把"解析出的帧"交给上层处理

namespace aegis::protocol {

// =============================================================================
// FrameCodec —— 流式帧解码器
//
// 解决的问题：TCP/串口提供的是"字节流"，没有消息边界。一次 read 可能得到：
//   (a) 半帧（分包）      —— 必须缓存起来，等后续字节到齐再解析
//   (b) 两帧粘在一起（粘包）—— 必须循环解析出每一帧
//   (c) 夹杂非法字节      —— 必须能跳过垃圾数据，重新找到帧头
//
// 做法：内部维护一个字节缓冲区 buffer_，每次收到数据就 append，
//       然后从缓冲区开头循环尝试解析完整帧，解析成功就回调、并丢弃已消费字节。
// =============================================================================
class FrameCodec {
public:
    // 上层传入的回调类型：每成功解析出一帧就调用一次。
    // 用 const Frame& 避免拷贝；回调内可以自行决定如何处理（入队、判值等）。
    using FrameHandler = std::function<void(const Frame&)>;

    // 输入一段新收到的字节；内部会处理粘包/分包，可能回调 0 次或多次。
    // data/size：字节首地址与长度；handler：解析成功后的回调。
    void feed(const std::uint8_t* data, std::size_t size, const FrameHandler& handler);

    // 清空内部缓冲与统计。断线重连后应调用，避免旧数据干扰新连接。
    void reset();

    // 把内存中的 Frame 编码成可发送的字节序列（帧头 + 长度 + 序号 + 类型 + 负载 + CRC）。
    // 声明为 static：编码不需要对象状态（不需要缓冲区），可以直接 FrameCodec::encode(...)。
    static std::vector<std::uint8_t> encode(const Frame& frame);

    // 下面三个是统计信息，界面上用于显示"收帧数 / 校验错误数 / 未解析残留字节"。
    std::uint64_t framesDecoded() const { return framesDecoded_; }
    std::uint64_t crcErrors() const { return crcErrors_; }
    std::size_t pendingBytes() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;    // 滑动缓冲区：存放尚未凑成完整帧的字节
    std::uint64_t framesDecoded_ = 0;     // 累计成功解析的帧数
    std::uint64_t crcErrors_ = 0;         // 累计 CRC 校验失败的次数
};

} // namespace aegis::protocol
