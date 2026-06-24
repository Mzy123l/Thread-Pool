#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <cmath>
#include <random>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <future>
#include <thread>
#include <atomic>

#include "../include/lock_free_thread_pool.hpp"          // 基于 std::function
#include "../include/lock_free_move_only_thread_pool.hpp" // 基于 move_only_function

// ======================== 有锁线程池 ========================
class LockBasedThreadPool
{
public:
    explicit LockBasedThreadPool(size_t numThreads = std::thread::hardware_concurrency())
        : stop_(false)
    {
        for (size_t i = 0; i < numThreads; ++i)
        {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~LockBasedThreadPool() { shutdown(); }

    template<typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args) -> std::future<typename std::invoke_result<Func, Args...>::type>
    {
        using ReturnType = typename std::invoke_result<Func, Args...>::type;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<ReturnType> res = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return res;
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_)
        {
            if (worker.joinable()) worker.join();
        }
    }

private:
    void workerLoop()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};

// ======================== 测试辅助函数 ========================

// CPU 密集型任务：计算斐波那契（递归慢版本，模拟重负载）
static int fib(int n)
{
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

// 轻量级任务：整数平方
static long long square(long long x)
{
    volatile long long y = x * x; // 防止优化
    return y;
}

// 模拟 I/O 等待（用 sleep_for 代替）
static void io_simulate(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 计时器类型
using Clock = std::chrono::high_resolution_clock;

struct TestResult
{
    double time_ms;
    size_t total_tasks;
    double throughput_per_sec;
};

// 通用测试函数：对给定的线程池执行一组任务，返回耗时
template<typename Pool>
TestResult run_test(Pool& pool, const std::vector<std::function<void()>>& tasks, int warmup = 0)
{
    // 预热
    for (int w = 0; w < warmup; ++w)
    {
        std::vector<std::future<void>> futs;
        for (const auto& t : tasks)
        {
            futs.push_back(pool.submit(t));
        }
        for (auto& f : futs) f.wait();
    }

    // 正式测试
    auto start = Clock::now();
    std::vector<std::future<void>> futures;
    futures.reserve(tasks.size());
    for (const auto& t : tasks)
    {
        futures.push_back(pool.submit(t));
    }
    for (auto& f : futures) f.wait();
    auto end = Clock::now();

    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    return { elapsed_ms, tasks.size(), tasks.size() / (elapsed_ms / 1000.0) };
}

// 生成任务列表
enum class TaskType { LIGHT, HEAVY, IO_MIX };

std::vector<std::function<void()>> generate_tasks(TaskType type, size_t count)
{
    std::vector<std::function<void()>> tasks;
    tasks.reserve(count);
    switch (type)
    {
    case TaskType::LIGHT:
        for (size_t i = 0; i < count; ++i)
            tasks.emplace_back([i]() { square(i % 1024); });
        break;
    case TaskType::HEAVY:
        for (size_t i = 0; i < count; ++i)
            tasks.emplace_back([]() { fib(25); }); // 每个约几毫秒
        break;
    case TaskType::IO_MIX:
        for (size_t i = 0; i < count; ++i)
        {
            if (i % 10 == 0)
                tasks.emplace_back([]() { io_simulate(1); }); // 模拟IO
            else
                tasks.emplace_back([i]() { square(i); });
        }
        break;
    }
    return tasks;
}

// ======================== main ========================
int main()
{
    const size_t NUM_THREADS = std::thread::hardware_concurrency();
    std::cout << "硬件线程数: " << NUM_THREADS << "\n\n";

    // 测试参数
    const size_t LIGHT_COUNT = 50000;
    const size_t HEAVY_COUNT = 200;
    const size_t IO_COUNT = 2000;
    const int WARMUP = 2;

    std::cout << "测试场景:\n";
    std::cout << "  1. 轻量任务 (square): " << LIGHT_COUNT << " 个\n";
    std::cout << "  2. 重量任务 (fib(25)): " << HEAVY_COUNT << " 个\n";
    std::cout << "  3. IO混合任务: " << IO_COUNT << " 个 (10% 模拟IO)\n\n";

    // 创建线程池
    LockBasedThreadPool lockPool(NUM_THREADS);
    thread_pool::LockFreeThreadPool<> freePool1(NUM_THREADS);
    thread_pool::LockFreeMoveOnlyThreadPool<> freePool2(NUM_THREADS);

    // 存储结果
    struct ScenarioResult
    {
        std::string name;
        double lock_time, free_func_time, free_move_time;
    };
    std::vector<ScenarioResult> results;

    // 测试各场景
    auto test_scenario = [&](const std::string& name, TaskType type, size_t count) {
        auto tasks = generate_tasks(type, count);

        // 有锁
        auto r_lock = run_test(lockPool, tasks, WARMUP);
        // 无锁 std::function
        auto r_free1 = run_test(freePool1, tasks, WARMUP);
        // 无锁 move_only_function
        auto r_free2 = run_test(freePool2, tasks, WARMUP);

        results.push_back({ name, r_lock.time_ms, r_free1.time_ms, r_free2.time_ms });

        std::cout << name << ":\n";
        std::cout << "  有锁线程池:       " << r_lock.time_ms << " ms, 吞吐量 "
            << r_lock.throughput_per_sec << " tasks/s\n";
        std::cout << "  无锁(function):   " << r_free1.time_ms << " ms, 吞吐量 "
            << r_free1.throughput_per_sec << " tasks/s\n";
        std::cout << "  无锁(move_only):  " << r_free2.time_ms << " ms, 吞吐量 "
            << r_free2.throughput_per_sec << " tasks/s\n\n";
        };

    test_scenario("轻量任务", TaskType::LIGHT, LIGHT_COUNT);
    test_scenario("重量任务", TaskType::HEAVY, HEAVY_COUNT);
    test_scenario("IO混合任务", TaskType::IO_MIX, IO_COUNT);

    // 打印汇总对比表
    std::cout << "\n========== 性能对比汇总 (耗时 ms) ==========\n";
    printf("%-20s %12s %12s %12s\n", "场景", "有锁", "无锁(function)", "无锁(move_only)");
    for (const auto& r : results)
    {
        printf("%-20s %12.2f %12.2f %12.2f\n", r.name.c_str(), r.lock_time, r.free_func_time, r.free_move_time);
    }

    // 计算加速比（相对于有锁）
    std::cout << "\n========== 加速比 (相对于有锁) ==========\n";
    printf("%-20s %12s %12s\n", "场景", "无锁(function)", "无锁(move_only)");
    for (const auto& r : results)
    {
        double speedup1 = r.lock_time / r.free_func_time;
        double speedup2 = r.lock_time / r.free_move_time;
        printf("%-20s %12.2fx %12.2fx\n", r.name.c_str(), speedup1, speedup2);
    }

    return 0;
}