#pragma once
// ============================================================
// lock_free_variant_thread_pool.hpp — std::variant 线程池
// ============================================================
// 使用 std::variant<PackagedTasks...> 消除运行时类型擦除，
// std::visit 编译期跳转表替代虚函数分发。
// 支持批量入队（BatchMode）和 CPU 亲和性（AffinityMode）。
//
// 用法：
//   using MyVariant = std::variant<
//       std::packaged_task<void()>,
//       std::packaged_task<int()>,
//       std::function<void()>   // 兜底类型
//   >;
//   VariantThreadPool<MyVariant> pool(4);
//   auto fut = pool.submit([] { return 42; });
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_thread_pool_base.hpp"
#include "lock_free_utility.hpp"

#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace thread_pool
{

template <typename VariantType,
          typename QueueType =
              lock_free_container::LockFreeQueue<VariantType>,
          BatchMode BatchV = BatchMode::Disabled,
          AffinityMode AffinityV = AffinityMode::Disabled>
class VariantThreadPool
    : public LockFreeThreadPoolBase<VariantType,
                                    VariantThreadPool<VariantType,
                                                      QueueType,
                                                      BatchV,
                                                      AffinityV>,
                                    QueueType,
                                    BatchV,
                                    AffinityV>
{
    using Base =
        LockFreeThreadPoolBase<VariantType,
                               VariantThreadPool<VariantType,
                                                 QueueType,
                                                 BatchV,
                                                 AffinityV>,
                               QueueType,
                               BatchV,
                               AffinityV>;
    friend Base;

public:
    using Base::Base;

    ~VariantThreadPool()
    {
        // batch flush removed
    }

    // ---- 提交任务 ----
    template <typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
        -> std::future<
            typename std::invoke_result<Func, Args...>::type>
    {
        using ReturnType =
            typename std::invoke_result<Func, Args...>::type;
        using PackagedTask = std::packaged_task<ReturnType()>;

        if constexpr (is_in_variant_v<PackagedTask, VariantType>)
        {
            constexpr std::size_t idx =
                variant_index_v<PackagedTask, VariantType>;

            PackagedTask pt(
                std::bind(std::forward<Func>(func),
                          std::forward<Args>(args)...));
            std::future<ReturnType> result = pt.get_future();

            auto variant_task =
                VariantType(std::in_place_index<idx>,
                            std::move(pt));

            if constexpr (BatchV == BatchMode::Enabled)
            {
                this->enqueue_task(std::move(variant_task));
            }
            else
            {
                this->enqueue_task(std::move(variant_task));
            }
            return result;
        }
        else if constexpr (is_in_variant_v<std::function<void()>,
                                           VariantType>)
        {
            constexpr std::size_t idx =
                variant_index_v<std::function<void()>,
                                VariantType>;

            auto pt = std::make_shared<PackagedTask>(
                std::bind(std::forward<Func>(func),
                          std::forward<Args>(args)...));
            std::future<ReturnType> result = pt->get_future();

            auto variant_task =
                VariantType(std::in_place_index<idx>,
                            std::function<void()>(
                                [pt]()
                                { (*pt)(); }));

            if constexpr (BatchV == BatchMode::Enabled)
            {
                this->enqueue_task(std::move(variant_task));
            }
            else
            {
                this->enqueue_task(std::move(variant_task));
            }
            return result;
        }
        else
        {
            static_assert(
                sizeof(ReturnType) == 0,
                "Return type not covered by variant. Add "
                "std::packaged_task<Ret()> to the variant, or "
                "include std::function<void()> as fallback.");
        }
    }

private:
    void execute_task(VariantType&& task)
    {
        std::visit(
            [](auto&& t)
            { std::forward<decltype(t)>(t)(); },
            std::move(task));
    }
};

}  // namespace thread_pool
