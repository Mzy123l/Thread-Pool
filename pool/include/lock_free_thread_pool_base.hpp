#pragma once
// ============================================================
// lock_free_thread_pool_base.hpp — 无锁线程池 CRTP 基类
// ============================================================
// 提取三种线程池变体的公共逻辑：
//   - 工作线程管理
//   - 任务队列交互（支持有界/无界队列）
//   - 统计计数器
//   - 生命周期控制（shutdown / wait_all）
//
// 派生类仅需实现 submit() 和 execute_task() 两个方法。
// 通过 CRTP 实现编译期多态，零虚函数开销。
//
// 分配器设计参照 std::priority_queue：
//   - allocator_type 从底层队列类型推导
//   - 构造时透传分配器实例给队列
//   - 与队列共享同一分配器（经 rebind 用于 packaged_task）
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_utility.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace thread_pool
{

// ============================================================
// LockFreeThreadPoolBase — CRTP 基类
// ============================================================
// 模板参数：
//   FuncType  — 队列元素类型（std::function<void()>、variant 等）
//   Derived   — CRTP 后代类（编译期多态）
//   QueueType — 无锁队列类型，默认 LockFreeQueue<FuncType>
//               分配器类型从 QueueType::allocator_type 推导
// ============================================================
template <typename FuncType, typename Derived,
          typename QueueType =
              lock_free_container::LockFreeQueue<FuncType>>
class LockFreeThreadPoolBase
{
public:
    using Task = FuncType;

    /// @brief 分配器类型，从队列类型推导（同 std::priority_queue 约定）
    using allocator_type = typename QueueType::allocator_type;

    // ---- 构造 ----
    // @param num_threads 工作线程数（默认 = 硬件并发数）
    // @param alloc       分配器实例，透传给底层队列
    explicit LockFreeThreadPoolBase(
        std::size_t num_threads =
            std::thread::hardware_concurrency(),
        const allocator_type& alloc = allocator_type())
        : task_queue_(alloc), task_allocator_(alloc), stop_{false},
          active_tasks_{0}, total_tasks_{0}, completed_tasks_{0}
    {
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
        {
            workers_.emplace_back(
                [this]
                { this->worker_thread(); });
        }
    }

    // ---- 禁止拷贝/移动 ----
    LockFreeThreadPoolBase(const LockFreeThreadPoolBase&) = delete;
    LockFreeThreadPoolBase& operator=(
        const LockFreeThreadPoolBase&) = delete;
    LockFreeThreadPoolBase(LockFreeThreadPoolBase&&) = delete;
    LockFreeThreadPoolBase& operator=(
        LockFreeThreadPoolBase&&) = delete;

    // ---- 析构 ----
    ~LockFreeThreadPoolBase() { shutdown(); }

    // ---- 等待所有活跃任务完成 ----
    void wait_all()
    {
        while (
            POOL_UNLIKELY(active_tasks_.load(std::memory_order_acquire)
                          > 0))
        {
            std::this_thread::yield();
        }
    }

    // ---- 优雅关闭：等待进行中任务完成，不再接受新任务 ----
    void shutdown()
    {
        stop_.store(true, std::memory_order_release);
        for (auto& worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    // ---- 立即关闭：丢弃所有未执行任务 ----
    void shutdown_now()
    {
        stop_.store(true, std::memory_order_release);

        Task dummy{};
        while (POOL_LIKELY(task_queue_.dequeue(dummy)))
        {
            active_tasks_.fetch_sub(1, std::memory_order_relaxed);
            total_tasks_.fetch_sub(1, std::memory_order_relaxed);
        }

        while (POOL_UNLIKELY(
            active_tasks_.load(std::memory_order_acquire) > 0))
        {
            std::this_thread::yield();
        }

        for (auto& worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    // ---- 统计查询 ----
    std::size_t active_count() const noexcept
    {
        return active_tasks_.load(std::memory_order_relaxed);
    }

    std::size_t total_count() const noexcept
    {
        return total_tasks_.load(std::memory_order_relaxed);
    }

    std::size_t completed_count() const noexcept
    {
        return completed_tasks_.load(std::memory_order_relaxed);
    }

    std::size_t thread_count() const noexcept
    {
        return workers_.size();
    }

    /// @brief 获取分配器副本（同 std::priority_queue 约定）
    allocator_type get_allocator() const noexcept
    {
        return task_allocator_;
    }

protected:
    // ---- 入队任务（供派生类 submit() 调用） ----
    // 重试语义：有界队列满时自旋等待。
    // 安全前提：队列的 enqueue 在失败时不得消费 task 实参。
    void enqueue_task(Task task)
    {
        total_tasks_.fetch_add(1, std::memory_order_relaxed);
        active_tasks_.fetch_add(1, std::memory_order_relaxed);

        while (POOL_UNLIKELY(!task_queue_.enqueue(std::move(task))))
        {
            std::this_thread::yield();
        }
    }

    // 数据成员（protected 允许派生类访问）
    // 注意：task_allocator_ 必须在 task_queue_ 之后声明，
    // 因为分配器由队列构造时同步初始化
    QueueType task_queue_;
    allocator_type task_allocator_;
    std::vector<std::thread> workers_;

    // 以下原子变量各自占据独立缓存行，消除 false sharing：
    // 轻量任务场景下，所有线程高频更新这些计数器，
    // 若在同一缓存行会导致互相 invalidate，线程越多越慢。
    alignas(lock_free_util::kCacheLineSize) std::atomic<bool> stop_;
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> active_tasks_;
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> total_tasks_;
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> completed_tasks_;

private:
    // ---- 工作线程主循环 ----
    // CRTP 分发点：dequeue 后调用 Derived::execute_task()
    void worker_thread()
    {
        Derived* derived = static_cast<Derived*>(this);

        while (POOL_UNLIKELY(
                   !stop_.load(std::memory_order_acquire))
               || active_tasks_.load(std::memory_order_acquire) > 0)
        {
            Task task{};
            if (POOL_LIKELY(task_queue_.dequeue(task)))
            {
                try
                {
                    derived->execute_task(std::move(task));
                }
                catch (...)
                {
                    // 异常由 packaged_task::get_future() 传播给调用方
                }
                completed_tasks_.fetch_add(1,
                                           std::memory_order_relaxed);
                active_tasks_.fetch_sub(1,
                                        std::memory_order_relaxed);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    }
};

}  // namespace thread_pool
