#pragma once
// ============================================================
// lock_free_dynamic_thread_pool.hpp — std::function 线程池
// ============================================================
// 使用 std::function<void()> 进行类型擦除，支持任意可调用对象 +
// 参数绑定。可通过 QueueType 模板参数替换底层队列实现。
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
    typename TaskAllocator = std::allocator<std::function<void()>>>
class DynamicThreadPool
    : public LockFreeThreadPoolBase<std::function<void()>,
                                    DynamicThreadPool<QueueType,
                                                      TaskAllocator>,
                                    QueueType, TaskAllocator>
{
    using Base =
        LockFreeThreadPoolBase<std::function<void()>,
                               DynamicThreadPool<QueueType,
                                                 TaskAllocator>,
                               QueueType, TaskAllocator>;
    friend Base;

public:
    using Base::Base;

    // ---- 提交任务 ----
    // 将可调用对象 + 参数包装为 std::function<void()> 后入队，
    // 返回 std::future<ReturnType>
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
                TaskAllocator>::template rebind_alloc<PackagedTask>;

        auto task = std::allocate_shared<PackagedTask>(
            PackagedTaskAllocator(this->task_allocator_),
            std::bind(std::forward<Func>(func),
                      std::forward<Args>(args)...));

        std::future<ReturnType> result = task->get_future();

        std::function<void()> wrapper = [task]()
        { (*task)(); };

        this->enqueue_task(std::move(wrapper));
        return result;
    }

private:
    void execute_task(std::function<void()>&& task)
    {
        task();
    }
};

// 向后兼容别名
template <typename TaskAllocator = std::allocator<std::function<void()>>>
using LockFreeThreadPool =
    DynamicThreadPool<lock_free_container::LockFreeQueue<
                          std::function<void()>>,
                      TaskAllocator>;

}  // namespace thread_pool
