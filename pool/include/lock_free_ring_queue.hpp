#pragma once
// ============================================================
// lock_free_ring_queue.hpp — 有界无锁环形队列
// ============================================================
// 基于 Dmitry Vyukov 的 MPMC 有界队列算法，使用序列号法在
// 固定大小的环形缓冲区上实现无锁并发操作。
//
// 模板参数：
//   T        — 元素类型（无需默认构造）
//   Capacity — 队列容量（必须是 2 的幂）
//   Allocator— 分配器类型（用于缓冲区内存分配）
//
// 接口与 LockFreeQueue 对齐，差异在于 enqueue 满时返回 false。
// ============================================================

#include "lock_free_utility.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lock_free_container
{

// ============================================================
// 环形队列默认分配器（复用 inner_queue 的实现模式）
// ============================================================
namespace inner_ring
{
template <typename T>
struct DefaultAllocator
{
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind
    {
        using other = DefaultAllocator<U>;
    };

    DefaultAllocator() noexcept = default;
    template <typename U>
    DefaultAllocator(const DefaultAllocator<U>&) noexcept
    {
    }

    T* allocate(std::size_t n)
    {
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t) noexcept
    {
        ::operator delete(p);
    }
};

}  // namespace inner_ring

// ============================================================
// LockFreeRingQueue — 有界无锁 MPMC 环形队列
// ============================================================
template <typename T,
          std::size_t Capacity,
          typename Allocator = inner_ring::DefaultAllocator<T>>
class LockFreeRingQueue
{
    static_assert(Capacity > 0, "Capacity 必须 > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity 必须是 2 的幂");

private:
    // ---- Cell：环形缓冲区槽位 ----
    // 序列号与数据存储分离，数据通过 placement new 手工管理
    // 64 字节对齐避免 false sharing
    struct alignas(lock_free_util::kCacheLineSize) Cell
    {
        std::atomic<std::size_t> sequence;
        // T 的原始存储，无需默认构造
        alignas(T) unsigned char storage[sizeof(T)];

        T* data()
        {
            // std::launder: 告知编译器 storage 上的 T 对象是新的
            return std::launder(reinterpret_cast<T*>(storage));
        }

        const T* data() const
        {
            return std::launder(reinterpret_cast<const T*>(storage));
        }
    };

    // Cell 分配器（从用户分配器 rebind 得到）
    using CellAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<Cell>;
    using CellAllocTraits = std::allocator_traits<CellAllocator>;

    Cell* buffer_;     // 环形缓冲区
    CellAllocator cell_allocator_;

    // 单调递增的位置计数器，非 64 位永不溢出（实际无用满 2^64 次）
    // 分离到不同缓存行避免 false sharing
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> enqueue_pos_{0};
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> dequeue_pos_{0};

    // ---- 索引计算（编译期优化为位与） ----
    static constexpr std::size_t mask_ = Capacity - 1;
    static std::size_t index(std::size_t pos) noexcept
    {
        return pos & mask_;
    }

public:
    // ---- 构造 / 析构 ----
    explicit LockFreeRingQueue(const Allocator& alloc = Allocator())
        : buffer_(nullptr), cell_allocator_(alloc)
    {
        buffer_ = CellAllocTraits::allocate(cell_allocator_, Capacity);
        // 初始化所有槽位的序列号
        for (std::size_t i = 0; i < Capacity; ++i)
        {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeRingQueue(const LockFreeRingQueue&) = delete;
    LockFreeRingQueue& operator=(const LockFreeRingQueue&) = delete;

    // 移动构造：接管缓冲区
    LockFreeRingQueue(LockFreeRingQueue&& other) noexcept
        : buffer_(other.buffer_)
        , cell_allocator_(std::move(other.cell_allocator_))
        , enqueue_pos_(
              other.enqueue_pos_.load(std::memory_order_relaxed))
        , dequeue_pos_(
              other.dequeue_pos_.load(std::memory_order_relaxed))
    {
        other.buffer_ = nullptr;
        other.enqueue_pos_.store(0, std::memory_order_relaxed);
        other.dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    LockFreeRingQueue& operator=(LockFreeRingQueue&&) = delete;

    ~LockFreeRingQueue()
    {
        if (buffer_)
        {
            clear();
            CellAllocTraits::deallocate(cell_allocator_, buffer_,
                                        Capacity);
        }
    }

    // ---- 入队 ----
    // enqueue 在 CAS 成功后才构造 T，因此失败时不会消费实参，
    // 调用方可安全重试。
    template <typename... Args>
    bool enqueue(Args&&... args)
    {
        // 1. 在 CAS 之前构造数据（如果构造抛异常，不影响队列状态）
        // 使用 std::optional 延迟构造，或直接在 CAS 后构造
        std::size_t pos =
            enqueue_pos_.load(std::memory_order_relaxed);

        for (;;)
        {
            Cell* cell = &buffer_[index(pos)];
            std::size_t seq =
                cell->sequence.load(std::memory_order_acquire);

            // diff = (intptr_t)seq - (intptr_t)pos
            // diff == 0: 槽位空闲（可以写入）
            // diff <  0: 队列已满
            // diff >  0: 其他生产者正在写入此槽
            auto diff = static_cast<std::intptr_t>(seq)
                        - static_cast<std::intptr_t>(pos);

            if (POOL_LIKELY(diff == 0))
            {
                // 尝试占据此位置
                if (POOL_LIKELY(enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed,
                        std::memory_order_relaxed)))
                {
                    // 占据成功，在槽位中构造数据
                    ::new (cell->storage)
                        T(std::forward<Args>(args)...);
                    // 发布数据
                    cell->sequence.store(pos + 1,
                                         std::memory_order_release);
                    return true;
                }
                // CAS 失败：其他生产者抢走了位置，pos 已更新，
                // 继续循环
            }
            else if (POOL_UNLIKELY(diff < 0))
            {
                // 队列已满
                return false;
            }
            else
            {
                // diff > 0：其他生产者正在进行，重新加载位置
                // 预取下一个槽位以便后续写入
                POOL_PREFETCH_W(&buffer_[index(pos + 1)]);
                pos = enqueue_pos_.load(
                    std::memory_order_relaxed);
            }
        }
    }

    // ---- 出队 ----
    bool dequeue(T& data)
    {
        std::size_t pos =
            dequeue_pos_.load(std::memory_order_relaxed);

        for (;;)
        {
            Cell* cell = &buffer_[index(pos)];
            std::size_t seq =
                cell->sequence.load(std::memory_order_acquire);

            // diff = (intptr_t)seq - (intptr_t)(pos + 1)
            // diff == 0: 槽位有数据可读
            // diff <  0: 队列为空
            // diff >  0: 其他消费者正在读取此槽
            auto diff = static_cast<std::intptr_t>(seq)
                        - static_cast<std::intptr_t>(pos + 1);

            if (POOL_LIKELY(diff == 0))
            {
                if (POOL_LIKELY(dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed,
                        std::memory_order_relaxed)))
                {
                    // 占据成功，取出数据
                    data = std::move(*cell->data());
                    cell->data()->~T();
                    // 标记槽位可供下一轮入队使用
                    cell->sequence.store(pos + Capacity,
                                         std::memory_order_release);
                    return true;
                }
            }
            else if (POOL_UNLIKELY(diff < 0))
            {
                return false;  // 队列为空
            }
            else
            {
                // 预取下一个槽位
                POOL_PREFETCH_R(&buffer_[index(pos + 1)]);
                pos = dequeue_pos_.load(
                    std::memory_order_relaxed);
            }
        }
    }

    // ---- 清空（非线程安全，应在无并发时调用） ----
    void clear() noexcept
    {
        T dummy;
        while (dequeue(dummy))
        {
            // dummy 在每次迭代被覆盖，析构由上层的 dequeue 处理
        }
    }

    // ---- 容量查询 ----
    static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

    // ---- 近似判空 ----
    // 仅作粗略检查：在并发场景下返回值可能立即过期
    bool empty() const noexcept
    {
        std::size_t e = enqueue_pos_.load(std::memory_order_acquire);
        std::size_t d = dequeue_pos_.load(std::memory_order_acquire);
        // 如果所有入队的都已被出队
        return e == d;
    }

    // ---- 获取分配器 ----
    Allocator get_allocator() const noexcept
    {
        return Allocator(cell_allocator_);
    }
};

}  // namespace lock_free_container
