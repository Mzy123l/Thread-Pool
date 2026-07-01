#pragma once
// ============================================================
// lock_free_strict_variant_thread_pool.hpp
// 严格 variant 线程池——自实现 packaged_task/future，
// 编译期 std::visit 分发，零虚表，零异常，零 RTTI。
//
// 与 VariantThreadPool 的区别：
//   - 不使用 std::packaged_task / std::future
//   - 自实现 StaticPromise / StaticFuture / StaticPackagedTask
//   - 不包含 std::function<void()> 保底类型
//   - submit() 返回类型必须精确匹配 variant 中的 Task 类型，
//     否则编译失败
//   - 兼容 -fno-rtti -fno-exceptions
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_thread_pool_base.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace thread_pool
{

// ============================================================
// StaticPromise<R> — 单次赋值的结果容器
// ============================================================
template <typename R>
class StaticPromise
{
public:
    StaticPromise() = default;

    void set_value(const R& value)
    {
        ::new (storage_) R(value);
        ready_.store(true, std::memory_order_release);
    }

    void set_value(R&& value)
    {
        ::new (storage_) R(std::move(value));
        ready_.store(true, std::memory_order_release);
    }

    R get_value()
    {
        wait();
        R* ptr = reinterpret_cast<R*>(storage_);
        R result = std::move(*ptr);
        ptr->~R();
        return result;
    }

    void wait() const noexcept
    {
        while (POOL_UNLIKELY(
            !ready_.load(std::memory_order_acquire)))
        {
            std::this_thread::yield();
        }
    }

    bool is_ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire);
    }

    // 返回内部存储指针（供 StaticPackagedTask 直接写入）
    void* result_ptr() noexcept { return storage_; }

    // 标记结果已就绪（写入存储后调用）
    void mark_ready() noexcept
    {
        ready_.store(true, std::memory_order_release);
    }

    StaticPromise(const StaticPromise&) = delete;
    StaticPromise& operator=(const StaticPromise&) = delete;
    StaticPromise(StaticPromise&&) = delete;
    StaticPromise& operator=(StaticPromise&&) = delete;

private:
    alignas(R) unsigned char storage_[sizeof(R)];
    std::atomic<bool> ready_{false};
};

// void 特化：仅同步，不存储值
template <>
class StaticPromise<void>
{
public:
    StaticPromise() = default;

    void set_value()
    {
        ready_.store(true, std::memory_order_release);
    }

    void get_value()
    {
        wait();
        ready_.store(false, std::memory_order_relaxed);
    }

    void wait() const noexcept
    {
        while (POOL_UNLIKELY(
            !ready_.load(std::memory_order_acquire)))
        {
            std::this_thread::yield();
        }
    }

    bool is_ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> ready_{false};
};

// ============================================================
// StaticFuture<R> — 用户持有的结果句柄
// ============================================================
template <typename R>
class StaticFuture
{
public:
    using PromisePtr = std::shared_ptr<StaticPromise<R>>;

    explicit StaticFuture(PromisePtr p) noexcept
        : promise_(std::move(p))
    {
    }

    /// @brief 阻塞等待并获取结果（仅可调用一次）
    R get()
    {
        return promise_->get_value();
    }

    /// @brief 非阻塞检查是否就绪
    bool is_ready() const noexcept
    {
        return promise_->is_ready();
    }

    /// @brief 阻塞等待结果就绪
    void wait() const noexcept
    {
        promise_->wait();
    }

    StaticFuture() = default;
    StaticFuture(StaticFuture&&) noexcept = default;
    StaticFuture& operator=(StaticFuture&&) noexcept = default;

private:
    PromisePtr promise_;
};

template <>
class StaticFuture<void>
{
public:
    using PromisePtr = std::shared_ptr<StaticPromise<void>>;

    explicit StaticFuture(PromisePtr p) noexcept
        : promise_(std::move(p))
    {
    }

    void get()
    {
        promise_->get_value();
    }

    bool is_ready() const noexcept
    {
        return promise_->is_ready();
    }

    void wait() const noexcept
    {
        promise_->wait();
    }

    StaticFuture() = default;
    StaticFuture(StaticFuture&&) noexcept = default;
    StaticFuture& operator=(StaticFuture&&) noexcept = default;

private:
    PromisePtr promise_;
};

// ============================================================
// StaticPackagedTask<R> — 自实现任务包装
// ============================================================
// 使用 std::function<R()> 做内部类型擦除（与 std::packaged_task
// 内部机制一致），配合自实现的 StaticPromise/StaticFuture。
// variant dispatch 仍为编译期 std::visit，零额外虚表。
// ============================================================
template <typename R>
class StaticPackagedTask
{
public:
    StaticPackagedTask() = default;

    template <typename F, typename... Args>
        requires std::is_invocable_r_v<R, F, Args...>
    explicit StaticPackagedTask(F&& f, Args&&... args)
        : promise_(std::make_shared<StaticPromise<R>>())
    {
        // 将 callable + 参数打包为 tuple，lambda 内 apply 调用
        using Bound = std::tuple<std::decay_t<F>,
                                 std::decay_t<Args>...>;
        auto bound = std::make_shared<Bound>(
            std::forward<F>(f), std::forward<Args>(args)...);

        func_ = [bound]() -> R
        {
            if constexpr (std::is_void_v<R>)
            {
                std::apply(
                    [](auto&& fn, auto&&... a)
                    { std::invoke(std::forward<decltype(fn)>(fn),
                                  std::forward<decltype(a)>(a)...); },
                    std::move(*bound));
            }
            else
            {
                return std::apply(
                    [](auto&& fn, auto&&... a)
                    { return std::invoke(
                          std::forward<decltype(fn)>(fn),
                          std::forward<decltype(a)>(a)...); },
                    std::move(*bound));
            }
        };
    }

    void operator()()
    {
        if (func_)
        {
            if constexpr (std::is_void_v<R>)
            {
                func_();
                promise_->set_value();
            }
            else
            {
                promise_->set_value(func_());
            }
        }
    }

    StaticFuture<R> get_future()
    {
        return StaticFuture<R>(promise_);
    }

    StaticPackagedTask(StaticPackagedTask&&) noexcept = default;
    StaticPackagedTask& operator=(
        StaticPackagedTask&&) noexcept = default;
    StaticPackagedTask(const StaticPackagedTask&) = delete;
    StaticPackagedTask& operator=(
        const StaticPackagedTask&) = delete;

    ~StaticPackagedTask() = default;

private:
    std::function<R()> func_;
    std::shared_ptr<StaticPromise<R>> promise_;
};

// ============================================================
// StrictVariantThreadPool
// ============================================================
// submit() 返回类型必须匹配 VariantType 中的 StaticPackagedTask<T()>，
// 否则 static_assert 编译失败。不包含保底类型。
// ============================================================
template <typename VariantType,
          typename QueueType =
              lock_free_container::LockFreeQueue<VariantType>,
          BatchMode BatchV = BatchMode::Disabled,
          AffinityMode AffinityV = AffinityMode::Disabled>
class StrictVariantThreadPool
    : public LockFreeThreadPoolBase<
          VariantType,
          StrictVariantThreadPool<VariantType, QueueType, BatchV,
                                  AffinityV>,
          QueueType,
          BatchV,
          AffinityV>
{
    using Base = LockFreeThreadPoolBase<
        VariantType,
        StrictVariantThreadPool<VariantType, QueueType, BatchV,
                                AffinityV>,
        QueueType,
        BatchV,
        AffinityV>;
    friend Base;

public:
    using Base::Base;

    ~StrictVariantThreadPool()
    {
        this->ensure_batch_flushed();
    }

    // ---- 提交任务 ----
    // 返回类型必须匹配 VariantType 中的 StaticPackagedTask<T()>。
    // 无保底措施——不匹配的返回类型直接编译失败。
    template <typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
    {
        using ReturnType =
            typename std::invoke_result<Func, Args...>::type;
        using TaskForReturn =
            StaticPackagedTask<ReturnType>;

        // 编译期检查：返回类型必须在 variant 中有对应的 task 类型
        static_assert(
            is_in_variant_v<TaskForReturn, VariantType>,
            "Return type not covered by variant. "
            "Add StaticPackagedTask<ReturnType()> to the variant.");

        constexpr std::size_t idx =
            variant_index_v<TaskForReturn, VariantType>;

        TaskForReturn task(std::forward<Func>(func),
                           std::forward<Args>(args)...);

        auto future = task.get_future();

        auto variant_task =
            VariantType(std::in_place_index<idx>,
                        std::move(task));

        if constexpr (BatchV == BatchMode::Enabled)
        {
            this->enqueue_task_batch(std::move(variant_task));
        }
        else
        {
            this->enqueue_task(std::move(variant_task));
        }
        return future;
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
