#pragma once
// ============================================================
// lock_free_dynamic_thread_pool.hpp — std::function 线程池
// ============================================================
// 使用 std::function<void()> 进行类型擦除，支持任意可调用对象 +
// 参数绑定。可通过 QueueType 模板参数替换底层队列实现。
// 支持批量入队（BatchMode）和 CPU 亲和性（AffinityMode）。
// 分配器类型从 QueueType::allocator_type 推导，
// 参照 std::priority_queue 设计。
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_thread_pool_base.hpp"

#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace thread_pool
{

template <
    typename QueueType =
        lock_free_container::LockFreeQueue<std::function<void()>>,
    BatchMode BatchV = BatchMode::Disabled,
    AffinityMode AffinityV = AffinityMode::Disabled>
class DynamicThreadPool
    : public LockFreeThreadPoolBase<std::function<void()>,
                                    DynamicThreadPool<QueueType,
                                                      BatchV,
                                                      AffinityV>,
                                    QueueType,
                                    BatchV,
                                    AffinityV>
{
    using Base =
        LockFreeThreadPoolBase<std::function<void()>,
                               DynamicThreadPool<QueueType,
                                                 BatchV,
                                                 AffinityV>,
                               QueueType,
                               BatchV,
                               AffinityV>;
    friend Base;

public:
    using Base::Base;

    // ---- 析构：确保批量缓冲区被清空 ----
    ~DynamicThreadPool()
    {
        this->ensure_batch_flushed();
    }

    // ---- 提交任务（多生产者安全） ----
    template <typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
        -> std::future<
            typename std::invoke_result<Func, Args...>::type>
    {
        using ReturnType =
            typename std::invoke_result<Func, Args...>::type;
        using PackagedTask = std::packaged_task<ReturnType()>;
        using PackagedTaskAllocator =
            typename std::allocator_traits<
                typename Base::allocator_type>::
                template rebind_alloc<PackagedTask>;

        auto task = std::allocate_shared<PackagedTask>(
            PackagedTaskAllocator(this->task_allocator_),
            std::bind(std::forward<Func>(func),
                      std::forward<Args>(args)...));

        std::future<ReturnType> result = task->get_future();

        std::function<void()> wrapper = [task]()
        { (*task)(); };

        if constexpr (BatchV == BatchMode::Enabled)
        {
            this->enqueue_task_batch(std::move(wrapper));
        }
        else
        {
            this->enqueue_task(std::move(wrapper));
        }
        return result;
    }

private:
    void execute_task(std::function<void()>&& task)
    {
        task();
    }
};

// 向后兼容别名（默认队列 + 默认选项）
using LockFreeThreadPool = DynamicThreadPool<>;

}  // namespace thread_pool
