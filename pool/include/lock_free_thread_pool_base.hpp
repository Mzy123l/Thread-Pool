#pragma once
// ============================================================
// lock_free_thread_pool_base.hpp — 无锁线程池 CRTP 基类
// ============================================================
// 提取三种线程池变体的公共逻辑：
//   - 工作线程管理 / 任务队列交互 / 统计 / 生命周期
//
// 模板参数：
//   FuncType  — 队列元素类型
//   Derived   — CRTP 后代类
//   QueueType — 无锁队列类型
//   BatchV    — 批量入队模式（Disabled / Enabled）
//   AffinityV — CPU 亲和性绑定（Disabled / Enabled）
//
// 派生类仅需实现 submit() 和 execute_task() 两个方法。
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_utility.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace thread_pool
{

// ============================================================
// 每线程统计槽位（独立缓存行，消除 false sharing）
// ============================================================
// 每个工作线程拥有独立的统计槽位，写入无原子操作开销，
// 查询时聚合所有槽位。
struct alignas(lock_free_util::kCacheLineSize) ThreadStatSlot
{
    std::atomic<std::size_t> tasks_completed{0};

    ThreadStatSlot() = default;
    ThreadStatSlot(ThreadStatSlot&& other) noexcept
        : tasks_completed(other.tasks_completed.load(
              std::memory_order_relaxed))
    {
    }
    ThreadStatSlot& operator=(ThreadStatSlot&& other) noexcept
    {
        tasks_completed.store(
            other.tasks_completed.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
        return *this;
    }
};

// ============================================================
// LockFreeThreadPoolBase — CRTP 基类
// ============================================================
template <typename FuncType, typename Derived,
          typename QueueType =
              lock_free_container::LockFreeQueue<FuncType>,
          BatchMode BatchV = BatchMode::Disabled,
          AffinityMode AffinityV = AffinityMode::Disabled>
class LockFreeThreadPoolBase
{
public:
    using Task = FuncType;
    using allocator_type = typename QueueType::allocator_type;

    // CAS 退避策略常量（高阈值避免过早退避）
    static constexpr int kPauseThreshold = 32;
    static constexpr int kYieldThreshold = 256;

    // ---- 构造：通过分配器 ----
    // @param num_threads 工作线程数（默认硬件并发数）
    // @param alloc       分配器，透传给底层队列
    // @param core_ids    CPU 亲和性核心列表（仅 AffinityMode::Enabled 时生效）
    explicit LockFreeThreadPoolBase(
        std::size_t num_threads =
            std::thread::hardware_concurrency(),
        const allocator_type& alloc = allocator_type(),
        std::vector<unsigned int> core_ids = {})
        : task_queue_(alloc), task_allocator_(alloc),
          worker_stats_(num_threads + 1),
          affinity_cores_(std::move(core_ids)),
          stop_{false},
          active_tasks_{0}, total_tasks_{0}, completed_tasks_{0}
    {
        init_workers(num_threads);
    }

    // ---- 构造：通过队列（拷贝） ----
    // @param num_threads 工作线程数（必填，无默认值以防歧义）
    // @param queue       队列副本
    // @param core_ids    CPU 亲和性核心列表
    explicit LockFreeThreadPoolBase(
        std::size_t num_threads,
        const QueueType& queue,
        std::vector<unsigned int> core_ids = {})
        : task_queue_(queue),
          task_allocator_(queue.get_allocator()),
          worker_stats_(num_threads + 1),
          affinity_cores_(std::move(core_ids)),
          stop_{false},
          active_tasks_{0}, total_tasks_{0}, completed_tasks_{0}
    {
        init_workers(num_threads);
    }

    // ---- 构造：通过队列（移动） ----
    // @param num_threads 工作线程数（必填）
    // @param queue       队列（移动语义）
    // @param core_ids    CPU 亲和性核心列表
    explicit LockFreeThreadPoolBase(
        std::size_t num_threads,
        QueueType&& queue,
        std::vector<unsigned int> core_ids = {})
        : task_queue_(std::move(queue)),
          task_allocator_(task_queue_.get_allocator()),
          worker_stats_(num_threads + 1),
          affinity_cores_(std::move(core_ids)),
          stop_{false},
          active_tasks_{0}, total_tasks_{0}, completed_tasks_{0}
    {
        init_workers(num_threads);
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
    // BatchMode::Enabled 时须先将缓冲区中的任务刷入队列，
    // 否则工作线程取不到任务，active_tasks_ 永远不会归零。
    void wait_all()
    {
        if constexpr (BatchV == BatchMode::Enabled)
        {
            flush_batch();
        }
        while (POOL_UNLIKELY(
            active_tasks_.load(std::memory_order_acquire) > 0))
        {
            std::this_thread::yield();
        }
    }

    // ---- 优雅关闭 ----
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
        // 聚合所有工作线程的完成数 + 关机时直接累加的值
        std::size_t sum = completed_tasks_.load(
            std::memory_order_relaxed);
        for (const auto& s : worker_stats_)
        {
            sum += s.tasks_completed.load(
                std::memory_order_relaxed);
        }
        return sum;
    }

    std::size_t thread_count() const noexcept
    {
        return workers_.size();
    }

    allocator_type get_allocator() const noexcept
    {
        return task_allocator_;
    }

protected:
    // ---- 渐进退避（非批量模式使用） ----
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
        else if (spin_count <= 128)
        {
            std::this_thread::sleep_for(
                std::chrono::microseconds(1));
        }
    }

    // ---- 单任务入队（带退避） ----
    // 多生产者安全：任何线程调用 submit() 均可正确入队。
    void enqueue_task(Task task)
    {
        total_tasks_.fetch_add(1, std::memory_order_relaxed);
        active_tasks_.fetch_add(1, std::memory_order_acquire);

        int spin = 0;
        while (POOL_UNLIKELY(
            !task_queue_.enqueue(std::move(task))))
        {
            backoff(spin++);
        }
    }

    // ---- 批量入队（BatchMode::Enabled 时使用） ----
    // 累积任务到本地缓冲区，满时批量提交到队列。
    // 减少 CAS 次数至原来的 1/N。
    static constexpr std::size_t kBatchSize = 64;
    Task batch_buffer_[kBatchSize];
    std::size_t batch_count_{0};

    void enqueue_task_batch(Task task)
    {
        total_tasks_.fetch_add(1, std::memory_order_relaxed);
        active_tasks_.fetch_add(1, std::memory_order_acquire);

        batch_buffer_[batch_count_++] = std::move(task);

        if (POOL_UNLIKELY(batch_count_ >= kBatchSize))
        {
            flush_batch();
        }
    }

    // 强制提交缓冲区中所有任务
    void flush_batch()
    {
        if (batch_count_ == 0) return;

        // 尝试批量入队
        size_t written = task_queue_.enqueue_batch(
            batch_buffer_, batch_count_);

        // 处理剩余未能入队的任务（队列满，逐个入队）
        for (size_t i = written; i < batch_count_; ++i)
        {
            int spin = 0;
            while (POOL_UNLIKELY(
                !task_queue_.enqueue(
                    std::move(batch_buffer_[i]))))
            {
                backoff(spin++);
            }
        }
        batch_count_ = 0;
    }

    // 确保析构前缓冲区被清空
    void ensure_batch_flushed()
    {
        if constexpr (BatchV == BatchMode::Enabled)
        {
            flush_batch();
        }
    }

    // 数据成员
    QueueType task_queue_;
    allocator_type task_allocator_;
    std::vector<std::thread> workers_;

    // 每线程统计槽位（槽位 0 保留，1..N 对应工作线程）
    std::vector<ThreadStatSlot> worker_stats_;

    // CPU 亲和性核心列表（空 = 自动分配 0,1,2,...）
    std::vector<unsigned int> affinity_cores_;

    // 全局原子计数器（生产者侧使用，保持简单）
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<bool> stop_;
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> active_tasks_;
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> total_tasks_;
    alignas(lock_free_util::kCacheLineSize)
        std::atomic<std::size_t> completed_tasks_;

private:
    // ---- 工作线程初始化（所有构造函数共用） ----
    void init_workers(std::size_t num_threads)
    {
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
        {
            workers_.emplace_back(
                [this, worker_id = i + 1]
                { this->worker_thread(worker_id); });
        }

        if constexpr (AffinityV == AffinityMode::Enabled)
        {
            apply_affinity();
        }
    }

    // ---- CPU 亲和性绑定 ----
    // 若用户指定了核心列表则使用之，否则自动分配 0,1,2,...
    void apply_affinity() noexcept
    {
#if defined(__linux__)
        if (workers_.empty()) return;

        // 确定要绑定的核心列表
        std::vector<unsigned int> cores;
        if (!affinity_cores_.empty())
        {
            cores = affinity_cores_;
        }
        else
        {
            unsigned int num_cores =
                std::thread::hardware_concurrency();
            for (unsigned int i = 0;
                 i < num_cores && i < workers_.size(); ++i)
            {
                cores.push_back(i);
            }
        }

        for (std::size_t i = 0; i < workers_.size(); ++i)
        {
            cpu_set_t thread_set;
            CPU_ZERO(&thread_set);
            unsigned int core = cores[i % cores.size()];
            CPU_SET(static_cast<int>(core), &thread_set);

            pthread_t native_handle =
                workers_[i].native_handle();
            pthread_setaffinity_np(
                native_handle, sizeof(cpu_set_t), &thread_set);
        }
#endif
        // 非 Linux 平台静默降级（无操作）
    }

    // ---- 工作线程主循环 ----
    void worker_thread(std::size_t worker_id)
    {
        Derived* derived = static_cast<Derived*>(this);
        ThreadStatSlot& my_stats = worker_stats_[worker_id];

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
                    // 异常由 packaged_task::get_future() 传播
                }
                // 写入本地统计槽位（无竞争，独立缓存行）
                my_stats.tasks_completed.fetch_add(
                    1, std::memory_order_relaxed);
                // release 确保 shutdown_now / wait_all
                // 的 acquire load 能看到递减
                active_tasks_.fetch_sub(
                    1, std::memory_order_release);
            }
            else
            {
                // 队列空，退避
                std::this_thread::yield();
            }
        }

        // 线程退出时，将本地计数归并到全局计数器
        std::size_t completed =
            my_stats.tasks_completed.load(
                std::memory_order_relaxed);
        completed_tasks_.fetch_add(
            completed, std::memory_order_relaxed);
        my_stats.tasks_completed.store(
            0, std::memory_order_relaxed);
    }
};

}  // namespace thread_pool
