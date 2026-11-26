// =============================================================================
// bench.cpp —— 纯计算链路的性能基准（不依赖界面）
//
// 目的：用可重复的数字回答两个问题：
//   1. 帧解码能撑住多高的采样率？
//   2. 环形缓冲的入队/出队开销是多少？
//
// 运行：build-mingw\bin\aegis_bench.exe
// 说明：这是微观基准，和界面绘制无关；界面 CPU 占用在 docs/性能测试报告.md 中单独说明。
// =============================================================================

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QVector>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "core/spsc_ring_buffer.h"
#include "protocol/frame.h"
#include "protocol/frame_codec.h"

namespace {

// 统计用的结构：一个采样块（与业务里的 SampleBlock 等价，但基准里不需要时间字段）
struct BenchBlock {
    float timeSec = 0.0f;
    std::array<std::int16_t, aegis::protocol::kDefaultChannelCount> values{};
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    using namespace aegis::protocol;

    // 构造一帧 10ms 的数据：100 个采样点 × 6 通道 = 600 个 int16。
    constexpr int kPointsPerFrame = 100;
    std::vector<std::int16_t> interleaved(static_cast<std::size_t>(kPointsPerFrame)
                                          * kDefaultChannelCount);
    for (std::size_t i = 0; i < interleaved.size(); ++i)
        interleaved[i] = static_cast<std::int16_t>((i * 7919) % 60000 - 30000);

    Frame frame;
    frame.type = FrameType::Data;
    frame.seq = 1;
    frame.payload = encodeDataPayload(0, interleaved.data(), interleaved.size());
    const auto bytes = FrameCodec::encode(frame);

    // ---- 基准 1：整帧解码 ----
    constexpr int kDecodeRounds = 2000;
    std::uint64_t sampleCount = 0;
    QElapsedTimer timer;
    timer.start();
    for (int round = 0; round < kDecodeRounds; ++round) {
        FrameCodec codec;
        codec.feed(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                   [&sampleCount](const Frame& decoded) {
                       DataPayload payload;
                       if (decodeDataPayload(decoded.payload, payload))
                           sampleCount += payload.samples.size();
                   });
    }
    const double decodeMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;

    std::printf("== 帧解码（%d 字节/帧, %d 次）==\n", static_cast<int>(bytes.size()),
                kDecodeRounds);
    std::printf("  总耗时        : %.2f ms\n", decodeMs);
    std::printf("  单帧耗时      : %.3f ms\n", decodeMs / kDecodeRounds);
    std::printf("  采样点吞吐    : %.2f M 点/秒\n",
                static_cast<double>(sampleCount) / (decodeMs / 1000.0) / 1.0e6);
    std::printf("  等价单核可支撑: %.1f kHz × %d 通道\n",
                static_cast<double>(sampleCount) / (decodeMs / 1000.0)
                    / static_cast<double>(kDefaultChannelCount) / 1000.0,
                static_cast<int>(kDefaultChannelCount));

    // ---- 基准 2：SPSC 环形缓冲 ----
    // 131072 × 16 字节 ≈ 2MB，必须放堆上：Windows 默认线程栈只有 1MB。
    auto ring = std::make_unique<aegis::core::SpscRingBuffer<BenchBlock, 131072>>();
    constexpr int kRingOps = 5000000;
    BenchBlock block;

    timer.restart();
    std::size_t popped = 0;
    BenchBlock out;
    for (int i = 0; i < kRingOps; ++i) {
        ring->push(block);
        popped += ring->pop(&out, 1);
    }
    const double ringMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;

    std::printf("\n== SPSC 环形缓冲（%d 次 push+pop）==\n", kRingOps);
    std::printf("  实际取出块数  : %zu\n", popped);
    std::printf("  总耗时        : %.2f ms\n", ringMs);
    std::printf("  单次 push+pop : %.1f ns\n",
                static_cast<double>(timer.nsecsElapsed()) / kRingOps);
    std::printf("  吞吐          : %.2f M 块/秒\n",
                static_cast<double>(kRingOps) / (ringMs / 1000.0) / 1.0e6);

    return 0;
}
