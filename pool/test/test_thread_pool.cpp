// ============================================================
// test_thread_pool.cpp — 线程池 GoogleTest TYPED_TEST
// ============================================================
// 覆盖所有池类型 × 队列类型的组合。

#include "../include/lock_free_thread_pool.hpp"
#include "../include/lock_free_ring_queue.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

#include <gtest/gtest.h>

// ============================================================
// 测试用类型定义
// ============================================================

// Variant 池的公共 variant 类型
using CommonVariant = std::variant<
    std::packaged_task<void()>,
    std::packaged_task<int()>,
    std::packaged_task<std::string()>,
    std::function<void()>  // 兜底
>;

// 链表队列别名（默认）
using FuncLinkedQ =
    lock_free_container::LockFreeQueue<std::function<void()>>;

// 环形队列别名
using FuncRingQ =
    lock_free_container::LockFreeRingQueue<std::function<void()>, 1024>;

// ============================================================
// 基础 DynamicThreadPool 测试（带队列组合）
// ============================================================

// 链表队列
using FuncLinkedPool = thread_pool::DynamicThreadPool<FuncLinkedQ>;
// 环形队列
using FuncRingPool = thread_pool::DynamicThreadPool<FuncRingQ>;
// Variant 池
using VariantPool = thread_pool::VariantThreadPool<CommonVariant>;

// 收集所有非 C++23 依赖的类型
using PoolTypes = ::testing::Types<
    FuncLinkedPool,
    FuncRingPool,
    VariantPool
>;

template <typename PoolType>
class ThreadPoolTest : public ::testing::Test
{
protected:
    PoolType pool{4};
};

TYPED_TEST_SUITE(ThreadPoolTest, PoolTypes);

// ---- 基本 submit/返回值 ----

TYPED_TEST(ThreadPoolTest, SubmitReturnsFuture)
{
    auto fut = this->pool.submit([] { return 1; });
    EXPECT_EQ(fut.get(), 1);
}

TYPED_TEST(ThreadPoolTest, ReturnValueCorrect)
{
    auto sum = this->pool.submit([](int a, int b) { return a + b; },
                                 10, 32);
    auto text = this->pool.submit(
        [] { return std::string("hello"); });

    EXPECT_EQ(sum.get(), 42);
    EXPECT_EQ(text.get(), "hello");
}

TYPED_TEST(ThreadPoolTest, BatchSubmit100)
{
    std::vector<std::future<int>> futures;
    for (int i = 1; i <= 100; ++i)
    {
        futures.emplace_back(
            this->pool.submit([i] { return i * i; }));
    }

    int sum = 0;
    for (auto& f : futures)
    {
        sum += f.get();
    }

    constexpr int expected = 100 * 101 * 201 / 6;  // Σ i²
    EXPECT_EQ(sum, expected);
}

TYPED_TEST(ThreadPoolTest, SubmitWithArgs)
{
    auto fut = this->pool.submit(
        [](int a, int b, int c) { return a * b + c; }, 3, 4, 5);
    EXPECT_EQ(fut.get(), 17);
}

// ---- 异常处理 ----

TYPED_TEST(ThreadPoolTest, ExceptionTask)
{
    auto failed = this->pool.submit(
        []() -> int
        { throw std::runtime_error("test error"); });

    auto ok = this->pool.submit([] { return 7; });

    bool caught = false;
    try
    {
        (void)failed.get();
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }

    EXPECT_TRUE(caught);
    EXPECT_EQ(ok.get(), 7);
}

// ---- 统计计数器 ----

TYPED_TEST(ThreadPoolTest, StatisticsCounters)
{
    auto f1 = this->pool.submit([] { return 1; });
    auto f2 = this->pool.submit([] { return 2; });

    f1.wait();
    f2.wait();
    this->pool.wait_all();

    EXPECT_EQ(this->pool.total_count(), 2);
    EXPECT_EQ(this->pool.completed_count(), 2);
    EXPECT_EQ(this->pool.active_count(), 0);
}

// ---- 线程数 ----

TYPED_TEST(ThreadPoolTest, ThreadCount)
{
    EXPECT_EQ(this->pool.thread_count(), 4);
}

// ---- shutdown 幂等性 ----

TYPED_TEST(ThreadPoolTest, IdempotentShutdown)
{
    this->pool.shutdown();

    TypeParam pool2(2);
    pool2.shutdown();
    pool2.shutdown();  // 二次 shutdown 不应崩溃
    SUCCEED();
}

// ---- shutdown_now ----

TYPED_TEST(ThreadPoolTest, ShutdownNowClearsQueue)
{
    TypeParam p(4);
    // 提交任务但不等待（降低内存占用）
    for (int i = 0; i < 200; ++i)
    {
        p.submit([] {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        });
    }
    // 立即关闭
    p.shutdown_now();
    // 不崩溃即为通过
    SUCCEED();
}

// ---- wait_all 阻塞语义 ----

TYPED_TEST(ThreadPoolTest, WaitAllBlocksUntilComplete)
{
    std::atomic<bool> done{false};
    this->pool.submit([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        done.store(true, std::memory_order_release);
    });

    this->pool.wait_all();
    EXPECT_TRUE(done.load(std::memory_order_acquire));
}

// ============================================================
// 动态创建的 pool（不同线程数）
// ============================================================

TEST(ThreadPoolCreationTest, DefaultThreadCount)
{
    thread_pool::DynamicThreadPool<> pool;
    EXPECT_GE(pool.thread_count(), 1);
}

TEST(ThreadPoolCreationTest, SingleThread)
{
    thread_pool::DynamicThreadPool<> pool(1);
    EXPECT_EQ(pool.thread_count(), 1);

    auto fut = pool.submit([] { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

// ============================================================
// move-only 类型测试（仅对 function 版 pool，lambda 捕获）
// ============================================================

TEST(ThreadPoolMoveOnlyLambdaTest, UniquePtrCapture)
{
    // std::function 不支持 unique_ptr 捕获，使用 shared_ptr 代替
    thread_pool::DynamicThreadPool<> pool(4);
    auto ptr = std::make_shared<int>(42);
    auto fut = pool.submit([ptr] { return *ptr + 1; });
    EXPECT_EQ(fut.get(), 43);
}

// ============================================================
// Ring buffer 队列线程池专项测试
// ============================================================

TEST(RingBufferPoolTest, SurvivesFullQueue)
{
    // 小容量环形队列 + 大量任务
    using SmallRingQ =
        lock_free_container::LockFreeRingQueue<std::function<void()>, 4>;
    thread_pool::DynamicThreadPool<SmallRingQ> pool(4);

    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;

    for (int i = 0; i < 200; ++i)
    {
        futures.push_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures) f.wait();
    pool.wait_all();

    EXPECT_EQ(counter.load(), 200);
}
