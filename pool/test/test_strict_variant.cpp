#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <variant>
#include <vector>

#include "../include/lock_free_strict_variant_thread_pool.hpp"
#include "../include/lock_free_ring_queue.hpp"

// ---- 测试用 task variant ----
// 仅包含需要返回类型的 StaticPackagedTask，无 std::function 保底
using StrictVariant = std::variant<
    thread_pool::StaticPackagedTask<void>,
    thread_pool::StaticPackagedTask<int>,
    thread_pool::StaticPackagedTask<long long>,
    thread_pool::StaticPackagedTask<std::string>>;

// ============================================================
// 基础功能
// ============================================================

TEST(StrictVariantPoolTest, SubmitReturnValue)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(2);

    auto fut = pool.submit([] { return 42; });
    EXPECT_EQ(fut.get(), 42);

    auto fut2 = pool.submit([](int x) { return x * x; }, 5);
    EXPECT_EQ(fut2.get(), 25);
}

TEST(StrictVariantPoolTest, SubmitLargeValue)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(2);

    auto fut = pool.submit(
        [] { return 1000000000000LL; });
    EXPECT_EQ(fut.get(), 1000000000000LL);
}

TEST(StrictVariantPoolTest, SubmitString)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(2);

    auto fut = pool.submit(
        [] { return std::string("hello world"); });
    EXPECT_EQ(fut.get(), "hello world");
}

TEST(StrictVariantPoolTest, SubmitVoid)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(2);
    std::atomic<int> counter{0};

    auto fut = pool.submit([&] { counter.fetch_add(1); });
    fut.get();
    EXPECT_EQ(counter.load(), 1);
}

// ============================================================
// 多任务
// ============================================================

TEST(StrictVariantPoolTest, BatchTasks)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(4);
    constexpr int N = 100;

    std::vector<thread_pool::StaticFuture<int>> futures;
    futures.reserve(N);
    for (int i = 0; i < N; ++i)
    {
        futures.push_back(
            pool.submit([](int x) { return x * x; }, i));
    }

    int sum = 0;
    for (int i = 0; i < N; ++i)
    {
        sum += futures[i].get();
    }

    // sum of squares: 0^2 + 1^2 + ... + 99^2
    constexpr int expected = N * (N - 1) * (2 * N - 1) / 6;
    EXPECT_EQ(sum, expected);
}

// ============================================================
// 环形队列
// ============================================================

TEST(StrictVariantPoolTest, WithRingQueue)
{
    using RingQ = lock_free_container::LockFreeRingQueue<
        StrictVariant, 1024>;

    thread_pool::StrictVariantThreadPool<StrictVariant, RingQ>
        pool(4);

    auto fut = pool.submit([] { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

// ============================================================
// 编译期检查：不匹配的返回类型应编译失败
// ============================================================

// 以下代码如果取消注释，应无法编译（double 不在 variant 中）
// TEST(StrictVariantPoolTest, CompileTimeCheck)
// {
//     pool.submit([] { return 3.14; });  // double not in variant
// }

// ============================================================
// 并发安全
// ============================================================

TEST(StrictVariantPoolTest, ConcurrentSubmit)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(4);
    constexpr int N = 500;
    std::atomic<int> counter{0};

    std::vector<thread_pool::StaticFuture<void>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; ++i)
    {
        futures.push_back(
            pool.submit([&] { counter.fetch_add(1); }));
    }

    for (auto& f : futures)
    {
        f.get();
    }
    EXPECT_EQ(counter.load(), N);
}

TEST(StrictVariantPoolTest, MixedTaskTypes)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(4);

    auto fi = pool.submit([] { return 10; });
    auto fs = pool.submit(
        [] { return std::string("test"); });
    auto fv = pool.submit([] {});

    EXPECT_EQ(fi.get(), 10);
    EXPECT_EQ(fs.get(), "test");
    fv.get();  // void
}

TEST(StrictVariantPoolTest, ShutdownNowBasic)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(2);
    auto fut = pool.submit([] { return 42; });
    // 等待任务完成
    fut.wait();
    pool.shutdown_now();
    EXPECT_EQ(fut.get(), 42);
}

TEST(StrictVariantPoolTest, ShutdownGraceful)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(2);
    auto fut = pool.submit([] { return 10; });
    pool.shutdown();  // 优雅关闭：等待所有任务完成
    EXPECT_EQ(fut.get(), 10);
}

// ============================================================
// move-only 捕获（lambda 值捕获）
// ============================================================

TEST(StrictVariantPoolTest, MoveOnlyCapture)
{
    thread_pool::StrictVariantThreadPool<StrictVariant> pool(2);

    auto ptr = std::make_unique<int>(42);
    auto fut = pool.submit(
        [p = std::move(ptr)]() -> int
        { return *p; });

    EXPECT_EQ(fut.get(), 42);
}
