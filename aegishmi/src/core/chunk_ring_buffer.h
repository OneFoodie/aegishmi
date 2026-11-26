#pragma once // 头文件保护

#include <algorithm> // std::min
#include <atomic>    // 原子索引
#include <cstddef>   // std::size_t
#include <cstdint>   // 定宽整数
#include <cstring>   // std::memcpy
#include <vector>    // 槽位存储

namespace aegis::core {

// =============================================================================
// ChunkRingBuffer —— 按"数据块"组织的单生产者/单消费者无锁环形缓冲
//
// 背景：原实现每个采样点一个 SampleBlock（float + array<int16,6>），
//       32/64 通道时每点膨胀到 132/260 字节，2 秒缓冲会吃掉上百 MB。
// 现实现：一个 Chunk = 一帧（若干采样点 × N 通道）的连续 int16 数据，
//       每块只在构造时分配一次内存，运行期只做 memcpy，避免逐点分配/拷贝。
//
// 容量与内存：
//   slot 数 × (maxPointsPerChunk × channelCount × 2 字节)
//   例：256 × (512 × 32 × 2) ≈ 8 MB/设备；6 通道时约 1.5 MB。
//
// 线程约定（与 SpscRingBuffer 相同）：
//   - 只能一个线程 push（收包线程），一个线程 consume（UI 定时器）；
//   - consume 回调里的 ChunkView 只在回调期间有效，回调返回后生产者可能复用该槽位。
// =============================================================================
struct ChunkView {
    std::uint32_t timestampMs = 0; // 本块第一个采样点的设备时间戳（毫秒）
    float startSec = 0.0f;         // 本块第一个采样点的显示时间（秒）
    int pointCount = 0;            // 采样点数
    int channelCount = 0;          // 通道数
    const std::int16_t* samples = nullptr; // 交叠排列：pointCount × channelCount
};

class ChunkRingBuffer {
public:
    ChunkRingBuffer(int channelCount, int maxPointsPerChunk, std::size_t slotCapacity)
        : channelCount_(std::max(1, channelCount))
        , maxPoints_(std::max(1, maxPointsPerChunk))
        , capacity_(nextPowerOfTwo(std::max<std::size_t>(2, slotCapacity)))
    {
        const std::size_t sampleCount =
            static_cast<std::size_t>(maxPoints_) * static_cast<std::size_t>(channelCount_);

        slots_.resize(capacity_);
        for (Slot& slot : slots_)
            slot.samples.resize(sampleCount);
    }

    // 生产者调用：拷贝一块数据。返回 false 表示缓冲已满或参数非法。
    bool push(std::uint32_t timestampMs, float startSec,
              const std::int16_t* interleaved, int pointCount)
    {
        if (interleaved == nullptr || pointCount <= 0 || pointCount > maxPoints_)
            return false;

        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_)
            return false; // 满：上层计入"丢块"

        Slot& slot = slots_[head & mask()];
        slot.timestampMs = timestampMs;
        slot.startSec = startSec;
        slot.pointCount = pointCount;

        const std::size_t bytes = static_cast<std::size_t>(pointCount)
                                  * static_cast<std::size_t>(channelCount_)
                                  * sizeof(std::int16_t);
        std::memcpy(slot.samples.data(), interleaved, bytes);

        // release：保证上面的数据写入对消费者可见。
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 消费者调用：依次处理当前可读的所有块。回调返回后 tail 前移，槽位可被复用。
    template <typename Fn>
    void consume(Fn&& fn)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        for (std::size_t index = tail; index < head; ++index) {
            Slot& slot = slots_[index & mask()];

            ChunkView view;
            view.timestampMs = slot.timestampMs;
            view.startSec = slot.startSec;
            view.pointCount = slot.pointCount;
            view.channelCount = channelCount_;
            view.samples = slot.samples.data();
            fn(view);
        }

        if (head != tail)
            tail_.store(head, std::memory_order_release);
    }

    // 近似可读块数（仅用于显示/调试）。
    std::size_t size() const
    {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    // 清空未读数据（只能在消费者线程调用）。
    void clear()
    {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

    int channelCount() const { return channelCount_; }
    int maxPointsPerChunk() const { return maxPoints_; }
    std::size_t capacity() const { return capacity_; }

private:
    struct Slot {
        std::uint32_t timestampMs = 0;
        float startSec = 0.0f;
        int pointCount = 0;
        std::vector<std::int16_t> samples;
    };

    static std::size_t nextPowerOfTwo(std::size_t value)
    {
        std::size_t result = 1;
        while (result < value)
            result <<= 1;
        return result;
    }

    // 容量是 2 的幂：index & (capacity-1) 等价于取模。
    std::size_t mask() const { return capacity_ - 1; }

    int channelCount_ = 1;
    int maxPoints_ = 1;
    std::size_t capacity_ = 2;
    std::vector<Slot> slots_;
    std::atomic<std::size_t> head_{0}; // 写索引（只增不减）
    std::atomic<std::size_t> tail_{0}; // 读索引（只增不减）
};

} // namespace aegis::core
