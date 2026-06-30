#pragma once
// ============================================================
// lock_free_thread_pool.hpp — 无锁线程池（三种变体）
// ============================================================
// 基于 CRTP 基类 LockFreeThreadPoolBase，提供三种线程池：
//
//   1. DynamicThreadPool
//      - 使用 std::function<void()> 进行类型擦除
//      - 支持任意可调用对象 + 参数绑定
//      - 默认选项，向后兼容（原 LockFreeThreadPool）
//
//   2. DynamicMoveOnlyThreadPool（需 C++23）
//      - 使用 std::move_only_function<void()> 进行类型擦除
//      - 支持捕获 move-only 对象（如 std::unique_ptr）
//
//   3. VariantThreadPool
//      - 使用 std::variant<PackagedTasks...> 消除运行时类型擦除
//      - std::visit 编译期跳转表替代虚函数分发
//      - 支持闭集的任务返回类型
//
// 所有变体均可通过 QueueType 模板参数替换底层队列实现。
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_ring_queue.hpp"
#include "lock_free_thread_pool_base.hpp"
#include "lock_free_utility.hpp"

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <version>

namespace thread_pool
{

// ============================================================
// 1. DynamicThreadPool — std::function 版本
// ============================================================
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
    // 让 Base 访问 execute_task
    friend Base;

public:
    using Base::Base;

    // ---- 提交任务 ----
    // 将可调用对象 + 参数绑定为 std::packaged_task，
    // 包装为 std::function<void()> 后入队。
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

        // 用分配器创建 packaged_task（allocate_shared）
        auto task = std::allocate_shared<PackagedTask>(
            PackagedTaskAllocator(this->task_allocator_),
            std::bind(std::forward<Func>(func),
                      std::forward<Args>(args)...));

        std::future<ReturnType> result = task->get_future();

        // 包装为 std::function<void()>
        std::function<void()> wrapper = [task]()
        { (*task)(); };

        this->enqueue_task(std::move(wrapper));
        return result;
    }

private:
    // CRTP 分发点：执行任务
    void execute_task(std::function<void()>&& task)
    {
        task();
    }
};

// ============================================================
// 2. DynamicMoveOnlyThreadPool — move_only_function 版本
// ============================================================
// 仅在编译器支持 std::move_only_function 时可用（C++23 或
// 部分 C++20 库扩展）
#if defined(__cpp_lib_move_only_function) || defined(_MSC_VER)

template <
    typename QueueType =
        lock_free_container::LockFreeQueue<
            std::move_only_function<void()>>,
    typename TaskAllocator =
        std::allocator<std::move_only_function<void()>>>
class DynamicMoveOnlyThreadPool
    : public LockFreeThreadPoolBase<
          std::move_only_function<void()>,
          DynamicMoveOnlyThreadPool<QueueType, TaskAllocator>,
          QueueType, TaskAllocator>
{
    using Base = LockFreeThreadPoolBase<
        std::move_only_function<void()>,
        DynamicMoveOnlyThreadPool<QueueType, TaskAllocator>,
        QueueType, TaskAllocator>;
    friend Base;

public:
    using Base::Base;

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

        // 包装为 move_only_function（支持 move-only 捕获）
        std::move_only_function<void()> wrapper = [task]()
        { (*task)(); };

        this->enqueue_task(std::move(wrapper));
        return result;
    }

private:
    void execute_task(std::move_only_function<void()>&& task)
    {
        task();
    }
};

#endif  // __cpp_lib_move_only_function

// ============================================================
// 3. VariantThreadPool — variant 版本（消除类型擦除）
// ============================================================
// 模板参数 VariantType 是一个 std::variant，其候选项为
// std::packaged_task<Ret()> 等可调用类型。
//
// 用法示例：
//   using MyVariant = std::variant<
//       std::packaged_task<void()>,
//       std::packaged_task<int()>,
//       std::function<void()>  // 兜底类型
//   >;
//   VariantThreadPool<MyVariant> pool(4);
//   auto fut = pool.submit([] { return 42; });
template <typename VariantType,
          typename QueueType =
              lock_free_container::LockFreeQueue<VariantType>,
          typename TaskAllocator = std::allocator<VariantType>>
class VariantThreadPool
    : public LockFreeThreadPoolBase<VariantType,
                                    VariantThreadPool<VariantType,
                                                      QueueType,
                                                      TaskAllocator>,
                                    QueueType, TaskAllocator>
{
    using Base =
        LockFreeThreadPoolBase<VariantType,
                               VariantThreadPool<VariantType,
                                                 QueueType,
                                                 TaskAllocator>,
                               QueueType, TaskAllocator>;
    friend Base;

public:
    using Base::Base;

    // ---- 提交任务 ----
    // 根据返回类型选择 variant 中的对应 packaged_task 候选项。
    // 若返回类型不在 variant 中且 variant 包含
    // std::function<void()>，则回退到 function 包装。
    template <typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
        -> std::future<
            typename std::invoke_result<Func, Args...>::type>
    {
        using ReturnType =
            typename std::invoke_result<Func, Args...>::type;
        using PackagedTask = std::packaged_task<ReturnType()>;

        // 编译期检查：目标 packaged_task 类型是否在 variant 中
        if constexpr (is_in_variant_v<PackagedTask, VariantType>)
        {
            // 直接路径：零类型擦除开销
            constexpr std::size_t idx =
                variant_index_v<PackagedTask, VariantType>;

            PackagedTask pt(
                std::bind(std::forward<Func>(func),
                          std::forward<Args>(args)...));
            std::future<ReturnType> result = pt.get_future();

            this->enqueue_task(
                VariantType(std::in_place_index<idx>,
                            std::move(pt)));
            return result;
        }
        else if constexpr (is_in_variant_v<std::function<void()>,
                                           VariantType>)
        {
            // 回退路径：用 std::function<void()> 包装
            constexpr std::size_t idx =
                variant_index_v<std::function<void()>,
                                VariantType>;

            auto pt = std::make_shared<PackagedTask>(
                std::bind(std::forward<Func>(func),
                          std::forward<Args>(args)...));
            std::future<ReturnType> result = pt->get_future();

            this->enqueue_task(
                VariantType(std::in_place_index<idx>,
                            std::function<void()>(
                                [pt]()
                                { (*pt)(); })));
            return result;
        }
        else
        {
            // 编译期报错：返回类型未被 variant 覆盖
            static_assert(
                sizeof(ReturnType) == 0,
                "Return type not covered by variant. Add "
                "std::packaged_task<Ret()> to the variant, or "
                "include std::function<void()> as fallback.");
        }
    }

private:
    // CRTP 分发点：通过 std::visit 编译期跳转表执行
    void execute_task(VariantType&& task)
    {
        std::visit(
            [](auto&& t)
            { std::forward<decltype(t)>(t)(); },
            std::move(task));
    }
};

// ============================================================
// 向后兼容别名
// ============================================================
// 保留原有类名 LockFreeThreadPool / LockFreeMoveOnlyThreadPool，
// 内部映射到新的 DynamicThreadPool / DynamicMoveOnlyThreadPool。

template <typename TaskAllocator = std::allocator<std::function<void()>>>
using LockFreeThreadPool =
    DynamicThreadPool<lock_free_container::LockFreeQueue<
                          std::function<void()>>,
                      TaskAllocator>;

#if defined(__cpp_lib_move_only_function) || defined(_MSC_VER)
template <
    typename TaskAllocator =
        std::allocator<std::move_only_function<void()>>>
using LockFreeMoveOnlyThreadPool =
    DynamicMoveOnlyThreadPool<
        lock_free_container::LockFreeQueue<
            std::move_only_function<void()>>,
        TaskAllocator>;
#endif

}  // namespace thread_pool
