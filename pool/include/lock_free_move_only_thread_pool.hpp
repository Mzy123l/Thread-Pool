#pragma once
// ============================================================
// lock_free_move_only_thread_pool.hpp — move_only_function 线程池
// ============================================================
// 使用 std::move_only_function<void()> 进行类型擦除，支持捕获
// move-only 对象（如 std::unique_ptr）。
// 需要 C++23 或支持 __cpp_lib_move_only_function 的编译器。
// 支持批量入队（BatchMode）和 CPU 亲和性（AffinityMode）。
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_thread_pool_base.hpp"

#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>
#include <version>

#if defined(__cpp_lib_move_only_function) || defined(_MSC_VER)

namespace thread_pool
{

template <
    typename QueueType =
        lock_free_container::LockFreeQueue<
            std::move_only_function<void()>>,
    BatchMode BatchV = BatchMode::Disabled,
    AffinityMode AffinityV = AffinityMode::Disabled>
class DynamicMoveOnlyThreadPool
    : public LockFreeThreadPoolBase<
          std::move_only_function<void()>,
          DynamicMoveOnlyThreadPool<QueueType,
                                     BatchV,
                                     AffinityV>,
          QueueType,
          BatchV,
          AffinityV>
{
    using Base = LockFreeThreadPoolBase<
        std::move_only_function<void()>,
        DynamicMoveOnlyThreadPool<QueueType,
                                   BatchV,
                                   AffinityV>,
        QueueType,
        BatchV,
        AffinityV>;
    friend Base;

public:
    using Base::Base;

    ~DynamicMoveOnlyThreadPool()
    {
        this->ensure_batch_flushed();
    }

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

        std::move_only_function<void()> wrapper = [task]()
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
    void execute_task(std::move_only_function<void()>&& task)
    {
        task();
    }
};

// 向后兼容别名
template <
    typename Alloc =
        std::allocator<std::move_only_function<void()>>>
using LockFreeMoveOnlyThreadPool =
    DynamicMoveOnlyThreadPool<
        lock_free_container::LockFreeQueue<
            std::move_only_function<void()>, Alloc>>;

}  // namespace thread_pool

#endif  // __cpp_lib_move_only_function
