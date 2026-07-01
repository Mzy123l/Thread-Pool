#pragma once
// ============================================================
// lock_free_strict_variant_thread_pool.hpp
// 严格 variant 线程池——自实现 packaged_task/future/function，
// 编译期 std::visit 分发，零虚表，零异常，零 RTTI。
//
// 与 VariantThreadPool 的区别：
//   - 不使用 std::packaged_task / std::future / std::function
//   - 自实现 StaticPromise / StaticFuture / StaticPackagedTask
//   - 自实现 StaticFunction（零虚表类型擦除，heap+SBO）
//   - 不包含 std::function<void()> 保底类型
//   - submit() 返回类型必须精确匹配 variant 中的 Task 类型，
//     否则编译失败
//   - 兼容 -fno-rtti -fno-exceptions
// ============================================================

#include "lock_free_queue.hpp"
#include "lock_free_thread_pool_base.hpp"

#include <atomic>
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

    void* result_ptr() noexcept { return storage_; }

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

    R get()
    {
        return promise_->get_value();
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
// StaticFunction<R> — 零虚表类型擦除的可调用包装
// ============================================================
// 使用函数指针 + void* 实现类型擦除，无虚函数、无 RTTI。
// 小对象优化（SBO）：≤48 字节的可调用对象存储在内部缓冲
// 区，避免堆分配。
// ============================================================
namespace detail
{

template <typename R>
struct InvokeRet
{
    template <typename F>
    static R call(void* data)
    {
        return (*static_cast<F*>(data))();
    }
};

template <>
struct InvokeRet<void>
{
    template <typename F>
    static void call(void* data)
    {
        (*static_cast<F*>(data))();
    }
};

}  // namespace detail

template <typename R>
class StaticFunction
{
    using InvokeFn = R (*)(void*);
    using DestroyFn = void (*)(void*);
    using MoveFn = void (*)(void* src, void* dst);

    static constexpr std::size_t kBufSize = 48;

    InvokeFn invoke_ = nullptr;
    DestroyFn destroy_ = nullptr;
    MoveFn move_ = nullptr;
    void* data_ = nullptr;

    alignas(std::max_align_t) unsigned char buf_[kBufSize]{};
    bool sbo_ = false;

public:
    StaticFunction() = default;

    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, StaticFunction>)
    explicit StaticFunction(F&& f)
    {
        using DecayF = std::decay_t<F>;

        if constexpr (sizeof(DecayF) <= kBufSize
                      && alignof(DecayF) <= alignof(std::max_align_t)
                      && std::is_nothrow_move_constructible_v<DecayF>)
        {
            // 小对象优化
            ::new (buf_) DecayF(std::forward<F>(f));
            data_ = buf_;
            sbo_ = true;

            invoke_ = &detail::InvokeRet<R>::template call<DecayF>;
            destroy_ = +[](void* d)
            { static_cast<DecayF*>(d)->~DecayF(); };
            move_ = +[](void* src, void* dst)
            {
                auto* s = static_cast<DecayF*>(src);
                ::new (dst) DecayF(std::move(*s));
                s->~DecayF();
            };
        }
        else
        {
            // 堆分配
            data_ = new DecayF(std::forward<F>(f));
            sbo_ = false;

            invoke_ = &detail::InvokeRet<R>::template call<DecayF>;
            destroy_ = +[](void* d)
            { delete static_cast<DecayF*>(d); };
            move_ = nullptr;  // 堆分配不需要 move
        }
    }

    ~StaticFunction()
    {
        if (data_ && destroy_) destroy_(data_);
    }

    StaticFunction(StaticFunction&& other) noexcept
        : invoke_(other.invoke_)
        , destroy_(other.destroy_)
        , move_(other.move_)
        , sbo_(other.sbo_)
    {
        if (other.sbo_ && other.data_ && move_)
        {
            // SBO：将 callable 移动到新对象缓冲区
            move_(other.data_, buf_);
            data_ = buf_;
        }
        else
        {
            data_ = other.data_;
        }

        other.invoke_ = nullptr;
        other.destroy_ = nullptr;
        other.move_ = nullptr;
        other.data_ = nullptr;
        other.sbo_ = false;
    }

    StaticFunction& operator=(StaticFunction&& other) noexcept
    {
        if (this != &other)
        {
            if (data_ && destroy_) destroy_(data_);

            invoke_ = other.invoke_;
            destroy_ = other.destroy_;
            move_ = other.move_;
            sbo_ = other.sbo_;

            if (other.sbo_ && other.data_ && move_)
            {
                move_(other.data_, buf_);
                data_ = buf_;
            }
            else
            {
                data_ = other.data_;
            }

            other.invoke_ = nullptr;
            other.destroy_ = nullptr;
            other.move_ = nullptr;
            other.data_ = nullptr;
            other.sbo_ = false;
        }
        return *this;
    }

    StaticFunction(const StaticFunction&) = delete;
    StaticFunction& operator=(const StaticFunction&) = delete;

    explicit operator bool() const noexcept
    {
        return invoke_ != nullptr;
    }

    R operator()()
    {
        return invoke_(data_);
    }
};

// ============================================================
// StaticPackagedTask<R> — 自实现任务包装（零 std::function）
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
        using Bound = std::tuple<std::decay_t<F>,
                                 std::decay_t<Args>...>;

        // 可拷贝 → 按值捕获（零额外堆分配）
        // 不可拷贝 → shared_ptr 包装
        if constexpr (std::is_copy_constructible_v<Bound>)
        {
            func_ = StaticFunction<R>(
                [bound = Bound(std::forward<F>(f),
                               std::forward<Args>(args)...)]() mutable -> R
                { return invoke_bound(std::move(bound)); });
        }
        else
        {
            auto bound = std::make_shared<Bound>(
                std::forward<F>(f), std::forward<Args>(args)...);
            func_ = StaticFunction<R>(
                [bound]() -> R
                { return invoke_bound(std::move(*bound)); });
        }
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
    template <typename Bound>
    static R invoke_bound(Bound&& bound)
    {
        return std::apply(
            [](auto&& fn, auto&&... a) -> decltype(auto)
            { return static_cast<decltype(fn)>(fn)(
                  static_cast<decltype(a)>(a)...); },
            std::forward<Bound>(bound));
    }

    StaticFunction<R> func_;
    std::shared_ptr<StaticPromise<R>> promise_;
};

// ============================================================
// StrictVariantThreadPool
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
        // batch flush removed
    }

    template <typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
    {
        using ReturnType =
            typename std::invoke_result<Func, Args...>::type;
        using TaskForReturn =
            StaticPackagedTask<ReturnType>;

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
            this->enqueue_task(std::move(variant_task));
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
