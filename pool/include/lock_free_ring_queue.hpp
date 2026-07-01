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
//   Allocator— 分配器类型
//
// 接口与 LockFreeQueue 对齐，enqueue 满时返回 false。
// 新增批量入队/出队（enqueue_batch / dequeue_batch），
// CAS 重试循环内建渐进退避策略。
// ============================================================

#include "lock_free_utility.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lock_free_container
{

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
public:
    using allocator_type = Allocator;

private:
    static_assert(Capacity > 0, "Capacity 必须 > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity 必须是 2 的幂");

    // CAS 退避策略常量
    static constexpr int kPauseThreshold = 8;    // 先 PAUSE 自旋
    static constexpr int kYieldThreshold = 32;   // 再 yield 让出

    struct alignas(lock_free_util::kCacheLineSize) Cell
    {
        std::atomic<std::size_t> sequence;
        alignas(T) unsigned char storage[sizeof(T)];

        T* data()
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }

        const T* data() const
        {
            return std::launder(reinterpret_cast<const T*>(storage));
        }
    };

    using CellAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<Cell>;
    using CellAllocTraits = std::allocator_traits<CellAllocator>;

    Cell* buffer_;
    CellAllocator cell_allocator_;

    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> enqueue_pos_{0};
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> dequeue_pos_{0};

    static constexpr std::size_t mask_ = Capacity - 1;
    static std::size_t index(std::size_t pos) noexcept
    {
        return pos & mask_;
    }

    // ---- 渐进退避（CAS 重试时调用） ----
    static void backoff(int spin_count) noexcept
    {
        if (spin_count <= kPauseThreshold)
        {
            POOL_PAUSE();
        }
        else if (spin_count <= kYieldThreshold)
        {
            std::this_thread::yield();
        }
        // 长时间仍失败则短暂休眠
        else if (spin_count <= 128)
        {
            std::this_thread::sleep_for(
                std::chrono::microseconds(1));
        }
    }

public:
    // ---- 构造 / 析构 ----
    explicit LockFreeRingQueue(const Allocator& alloc = Allocator())
        : buffer_(nullptr), cell_allocator_(alloc)
    {
        buffer_ = CellAllocTraits::allocate(cell_allocator_, Capacity);
        for (std::size_t i = 0; i < Capacity; ++i)
        {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeRingQueue(const LockFreeRingQueue&) = delete;
    LockFreeRingQueue& operator=(const LockFreeRingQueue&) = delete;

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

    // ---- 单元素入队（带 CAS 退避） ----
    template <typename... Args>
    bool enqueue(Args&&... args)
    {
        std::size_t pos =
            enqueue_pos_.load(std::memory_order_relaxed);
        int spin = 0;

        for (;;)
        {
            Cell* cell = &buffer_[index(pos)];
            std::size_t seq =
                cell->sequence.load(std::memory_order_acquire);

            auto diff = static_cast<std::intptr_t>(seq)
                        - static_cast<std::intptr_t>(pos);

            if (POOL_LIKELY(diff == 0))
            {
                if (POOL_LIKELY(enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed,
                        std::memory_order_relaxed)))
                {
                    ::new (cell->storage)
                        T(std::forward<Args>(args)...);
                    cell->sequence.store(pos + 1,
                                         std::memory_order_release);
                    return true;
                }
                // CAS 失败 → 退避后重试
                backoff(spin++);
            }
            else if (POOL_UNLIKELY(diff < 0))
            {
                return false;  // 队列满，不重试
            }
            else
            {
                // 其他生产者领先，刷新 pos
                POOL_PREFETCH_W(&buffer_[index(pos + 1)]);
                pos = enqueue_pos_.load(
                    std::memory_order_relaxed);
                spin = 0;  // 重置计数（非同一槽位竞争）
            }
        }
    }

    // ---- 单元素出队（带 CAS 退避） ----
    bool dequeue(T& data)
    {
        std::size_t pos =
            dequeue_pos_.load(std::memory_order_relaxed);
        int spin = 0;

        for (;;)
        {
            Cell* cell = &buffer_[index(pos)];
            std::size_t seq =
                cell->sequence.load(std::memory_order_acquire);

            auto diff = static_cast<std::intptr_t>(seq)
                        - static_cast<std::intptr_t>(pos + 1);

            if (POOL_LIKELY(diff == 0))
            {
                if (POOL_LIKELY(dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed,
                        std::memory_order_relaxed)))
                {
                    data = std::move(*cell->data());
                    cell->data()->~T();
                    cell->sequence.store(pos + Capacity,
                                         std::memory_order_release);
                    return true;
                }
                // CAS 失败 → 退避后重试
                backoff(spin++);
            }
            else if (POOL_UNLIKELY(diff < 0))
            {
                return false;  // 队列空
            }
            else
            {
                POOL_PREFETCH_R(&buffer_[index(pos + 1)]);
                pos = dequeue_pos_.load(
                    std::memory_order_relaxed);
                spin = 0;
            }
        }
    }

    // ---- 批量入队 ----
    // 原子地预留 count 个连续槽位并写入数据。
    // 若可用槽位不足 count，写入 min(count, available) 个。
    // 返回实际入队数量（可能 < count）。
    // 不会丢失任务：要么全部预留成功，要么预留实际可用数量。
    size_t enqueue_batch(T* items, size_t count)
    {
        if (POOL_UNLIKELY(count == 0)) return 0;

        std::size_t pos =
            enqueue_pos_.load(std::memory_order_relaxed);
        int spin = 0;

        for (;;)
        {
            // 检查连续可用槽位数
            size_t avail = 0;
            for (size_t i = 0; i < count; ++i)
            {
                Cell* cell = &buffer_[index(pos + i)];
                std::size_t seq =
                    cell->sequence.load(std::memory_order_acquire);
                if (seq != pos + i) break;  // 不可用
                ++avail;
            }
            if (avail == 0) return 0;  // 队列满

            // 原子预留 avail 个槽位
            if (POOL_LIKELY(enqueue_pos_.compare_exchange_weak(
                    pos, pos + avail, std::memory_order_relaxed,
                    std::memory_order_relaxed)))
            {
                // 写入数据并发布序列号
                for (size_t i = 0; i < avail; ++i)
                {
                    Cell* cell = &buffer_[index(pos + i)];
                    ::new (cell->storage)
                        T(std::move(items[i]));
                    cell->sequence.store(pos + i + 1,
                                         std::memory_order_release);
                }
                return avail;
            }
            // CAS 失败 → 退避
            backoff(spin++);
        }
    }

    // ---- 批量出队 ----
    // 原子地预留 count 个连续槽位并读取数据。
    // 若可用数据不足 count，读取 min(count, available) 个。
    // 返回实际出队数量（可能 < count）。
    size_t dequeue_batch(T* items, size_t count)
    {
        if (POOL_UNLIKELY(count == 0)) return 0;

        std::size_t pos =
            dequeue_pos_.load(std::memory_order_relaxed);
        int spin = 0;

        for (;;)
        {
            // 检查连续可用数据槽位数
            size_t avail = 0;
            for (size_t i = 0; i < count; ++i)
            {
                Cell* cell = &buffer_[index(pos + i)];
                std::size_t seq =
                    cell->sequence.load(std::memory_order_acquire);
                if (seq != pos + i + 1) break;  // 无数据
                ++avail;
            }
            if (avail == 0) return 0;  // 队列空

            if (POOL_LIKELY(dequeue_pos_.compare_exchange_weak(
                    pos, pos + avail, std::memory_order_relaxed,
                    std::memory_order_relaxed)))
            {
                for (size_t i = 0; i < avail; ++i)
                {
                    Cell* cell = &buffer_[index(pos + i)];
                    items[i] = std::move(*cell->data());
                    cell->data()->~T();
                    cell->sequence.store(pos + i + Capacity,
                                         std::memory_order_release);
                }
                return avail;
            }
            backoff(spin++);
        }
    }

    // ---- 清空（非线程安全） ----
    void clear() noexcept
    {
        T dummy;
        while (dequeue(dummy)) {}
    }

    static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

    bool empty() const noexcept
    {
        std::size_t e = enqueue_pos_.load(std::memory_order_acquire);
        std::size_t d = dequeue_pos_.load(std::memory_order_acquire);
        return e == d;
    }

    Allocator get_allocator() const noexcept
    {
        return Allocator(cell_allocator_);
    }
};

}  // namespace lock_free_container
