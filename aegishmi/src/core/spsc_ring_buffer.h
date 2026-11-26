#pragma once // 头文件保护

#include <algorithm> // std::min
#include <array>     // std::array：编译期定长数组，作为环形缓冲区底层存储
#include <atomic>    // std::atomic：原子变量，保证多线程读写索引时不出现数据竞争
#include <cstddef>   // std::size_t

namespace aegis::core {

// =============================================================================
// SpscRingBuffer —— 单生产者/单消费者无锁环形缓冲
//
// 使用场景：一个线程（生产者，收包线程）不停写入采样点，
//           另一个线程（消费者，UI 线程）定时把数据取走画图。
//
// "环形"：底层数组首尾相接，写满后回到下标 0 继续写（不搬移数据）。
// "无锁"：只用两个原子索引 head_（写入位置）和 tail_（读取位置），
//         配合合适的 memory order，避免互斥锁带来的阻塞和优先级反转。
//
// 前提：只能有一个线程调用 push，另一个线程调用 pop。多生产者/多消费者不适用。
// =============================================================================
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    // 容量必须 ≥ 2，否则 head/tail 相减没有意义。
    static_assert(Capacity >= 2, "capacity must be >= 2");
    // 容量必须是 2 的幂：这样可以把"取模"运算 (index % Capacity) 优化成位与 (index & (Capacity-1))，
    // 位与是常数时间，且避免了负数取模等问题。
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    // 生产者调用：把 value 写入队尾。
    // 返回 false 表示缓冲区已满，本次写入失败（调用方应计数"丢块"）。
    bool push(const T& value)
    {
        // relaxed：head_ 只有生产者自己会修改，此处只需要读到最新值，不必同步其他内存。
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // acquire：读取消费者发布的 tail_。acquire 保证"消费者已释放的槽位"对生产者的后续写入可见。
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        // head - tail 就是当前元素个数（两个索引只增不减，自然溢出，无符号减法依然正确）。
        if (head - tail >= Capacity)
            return false; // 满了

        // 写入数据。& kMask 等价于 % Capacity，但更快（因为容量是 2 的幂）。
        buffer_[head & kMask] = value;

        // release：先保证上面的数据写入完成，再把 head_ 发布出去，
        // 这样消费者一旦看到新的 head_，就一定能看到写好的数据。
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 消费者调用：最多取走 maxCount 个元素放到 out 数组，返回实际取走的个数。
    std::size_t pop(T* out, std::size_t maxCount)
    {
        // relaxed：tail_ 只有消费者自己修改。
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // acquire：看到生产者发布的 head_，从而保证读到的槽位数据是最新的。
        const std::size_t head = head_.load(std::memory_order_acquire);

        const std::size_t available = head - tail;       // 当前可读元素个数
        const std::size_t count = std::min(available, maxCount); // 不能超过调用方给的容量

        // 按顺序拷贝到调用方提供的数组。
        for (std::size_t i = 0; i < count; ++i)
            out[i] = buffer_[(tail + i) & kMask];

        // release：数据已经读完，发布新的 tail_，允许生产者复用这些槽位。
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // 近似大小：两个索引都按 acquire 读取。只用于显示/调试，不适合做精确同步判断。
    std::size_t size() const
    {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    // 容量（编译期常量）。
    constexpr std::size_t capacity() const { return Capacity; }

    // 清空：把 tail 推到 head 的位置，相当于丢弃所有未读数据。
    // 只能在消费者线程调用，否则会与 pop 竞争。
    void clear()
    {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    // 掩码：Capacity 是 2 的幂，所以 Capacity-1 的二进制是"低位全 1"，
    // 例如 Capacity=8 -> kMask=7 (0b111)，index & 7 就是 index % 8。
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};          // 底层存储；{} 表示值初始化
    std::atomic<std::size_t> head_{0};          // 写索引：只增不减
    std::atomic<std::size_t> tail_{0};          // 读索引：只增不减
};

} // namespace aegis::core
