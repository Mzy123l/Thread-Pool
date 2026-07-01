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
#include <variant>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <string>

#include "../include/lock_free_thread_pool.hpp"
#include "../include/lock_free_move_only_thread_pool.hpp"
#include "../include/lock_free_variant_thread_pool.hpp"
#include "../include/lock_free_ring_queue.hpp"

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

// 轻量级任务：整数平方（每个任务使用独立存储，避免 false sharing）
static long long square(long long x)
{
    volatile long long y = x * x;
    return y;
}

// 模拟 I/O 等待
static void io_simulate(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ======================== 类型定义 ========================

using Clock = std::chrono::high_resolution_clock;

// Variant 线程池的基准测试类型
using BenchVariant = std::variant<
    std::packaged_task<void()>,
    std::packaged_task<int()>,
    std::packaged_task<long long()>,
    std::function<void()>
>;

// 环形队列容量（须为 2 的幂）
constexpr std::size_t RING_CAPACITY = 65536;

// 环形队列类型别名
using RingFuncQ = lock_free_container::LockFreeRingQueue<
    std::function<void()>, RING_CAPACITY>;
using RingMoveQ = lock_free_container::LockFreeRingQueue<
    std::move_only_function<void()>, RING_CAPACITY>;
using RingVariantQ = lock_free_container::LockFreeRingQueue<
    BenchVariant, RING_CAPACITY>;

// 链表队列类型（显式指定，用于对比）
using LinkedFuncQ = lock_free_container::LockFreeQueue<
    std::function<void()>>;
using LinkedMoveQ = lock_free_container::LockFreeQueue<
    std::move_only_function<void()>>;
using LinkedVariantQ = lock_free_container::LockFreeQueue<BenchVariant>;

// ======================== 数据结构 ========================

struct IterResult
{
    double time_ms;
    size_t total_tasks;
    double throughput_per_sec;
};

struct PoolResult
{
    std::string pool_name;
    std::vector<double> times;  // 每次迭代的耗时 ms
    double avg_ms = 0;
    double stddev_ms = 0;
    double throughput = 0;
};

struct ScenarioResult
{
    std::string scenario_name;
    size_t thread_count;
    size_t task_count;
    std::vector<PoolResult> pool_results;
    // 相对于有锁的加速比
    double lock_avg = 0;
};

// ======================== 统计计算 ========================

double compute_avg(const std::vector<double>& values)
{
    if (values.empty()) return 0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double compute_stddev(const std::vector<double>& values, double avg)
{
    if (values.size() < 2) return 0;
    double sum_sq = 0;
    for (double v : values)
    {
        double diff = v - avg;
        sum_sq += diff * diff;
    }
    return std::sqrt(sum_sq / (values.size() - 1));
}

// 判断数据是否稳定（变异系数 < 10%）
bool is_stable(const std::vector<double>& values, double avg)
{
    if (avg <= 0) return true;
    double sd = compute_stddev(values, avg);
    return (sd / avg) < 0.10;
}

// ======================== 任务生成 ========================

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
            tasks.emplace_back([]() { fib(25); });
        break;
    case TaskType::IO_MIX:
        for (size_t i = 0; i < count; ++i)
        {
            if (i % 10 == 0)
                tasks.emplace_back([]() { io_simulate(1); });
            else
                tasks.emplace_back([i]() { square(i); });
        }
        break;
    }
    return tasks;
}

// ======================== 通用测试函数 ========================

template<typename Pool>
IterResult run_test_once(Pool& pool, const std::vector<std::function<void()>>& tasks)
{
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

// 预热 + 多次测试
template<typename Pool>
PoolResult run_test_repeated(
    Pool& pool,
    const std::string& pool_name,
    const std::vector<std::function<void()>>& tasks,
    int iterations,
    int warmup)
{
    PoolResult result;
    result.pool_name = pool_name;

    // 预热
    for (int w = 0; w < warmup; ++w)
    {
        std::vector<std::future<void>> futs;
        futs.reserve(tasks.size());
        for (const auto& t : tasks)
            futs.push_back(pool.submit(t));
        for (auto& f : futs) f.wait();
    }

    // 正式测试
    result.times.reserve(iterations);
    for (int i = 0; i < iterations; ++i)
    {
        IterResult r = run_test_once(pool, tasks);
        result.times.push_back(r.time_ms);
    }

    result.avg_ms = compute_avg(result.times);
    result.stddev_ms = compute_stddev(result.times, result.avg_ms);
    result.throughput = tasks.size() / (result.avg_ms / 1000.0);

    return result;
}

// ======================== 场景测试 ========================

struct TestConfig
{
    std::string name;
    TaskType type;
    size_t count;
};

ScenarioResult test_scenario(
    const TestConfig& config,
    size_t num_threads,
    int iterations,
    int warmup)
{
    ScenarioResult scenario;
    scenario.scenario_name = config.name;
    scenario.thread_count = num_threads;
    scenario.task_count = config.count;

    auto tasks = generate_tasks(config.type, config.count);

    // ---- 有锁线程池 ----
    {
        LockBasedThreadPool pool(num_threads);
        auto r = run_test_repeated(pool, "有锁(std::queue)", tasks, iterations, warmup);
        scenario.pool_results.push_back(std::move(r));
        scenario.lock_avg = r.avg_ms;
        pool.shutdown();
    }

    // ---- 无锁 function（链表队列） ----
    {
        thread_pool::DynamicThreadPool<LinkedFuncQ> pool(num_threads);
        auto r = run_test_repeated(pool, "无锁(function+链表)", tasks, iterations, warmup);
        scenario.pool_results.push_back(std::move(r));
    }

    // ---- 无锁 function（环形队列） ----
    {
        thread_pool::DynamicThreadPool<RingFuncQ> pool(num_threads);
        auto r = run_test_repeated(pool, "无锁(function+环形)", tasks, iterations, warmup);
        scenario.pool_results.push_back(std::move(r));
    }

    // ---- 无锁 move_only（链表队列） ----
    {
        thread_pool::DynamicMoveOnlyThreadPool<LinkedMoveQ> pool(num_threads);
        auto r = run_test_repeated(pool, "无锁(move_only+链表)", tasks, iterations, warmup);
        scenario.pool_results.push_back(std::move(r));
    }

    // ---- 无锁 move_only（环形队列） ----
    {
        thread_pool::DynamicMoveOnlyThreadPool<RingMoveQ> pool(num_threads);
        auto r = run_test_repeated(pool, "无锁(move_only+环形)", tasks, iterations, warmup);
        scenario.pool_results.push_back(std::move(r));
    }

    // ---- 无锁 variant（链表队列） ----
    {
        thread_pool::VariantThreadPool<BenchVariant, LinkedVariantQ> pool(num_threads);
        auto r = run_test_repeated(pool, "无锁(variant+链表)", tasks, iterations, warmup);
        scenario.pool_results.push_back(std::move(r));
    }

    // ---- 无锁 variant（环形队列） ----
    {
        thread_pool::VariantThreadPool<BenchVariant, RingVariantQ> pool(num_threads);
        auto r = run_test_repeated(pool, "无锁(variant+环形)", tasks, iterations, warmup);
        scenario.pool_results.push_back(std::move(r));
    }

    return scenario;
}

// ======================== 输出格式化 ========================

void print_separator(int width = 110)
{
    std::cout << std::string(width, '=') << "\n";
}

void print_dashed(int width = 110)
{
    std::cout << std::string(width, '-') << "\n";
}

// 打印单个场景的结果表
void print_scenario_table(const ScenarioResult& scenario)
{
    std::cout << "\n场景: " << scenario.scenario_name
              << "  |  线程数: " << scenario.thread_count
              << "  |  任务数: " << scenario.task_count << "\n";
    print_dashed();

    // 表头
    std::cout << std::left
              << std::setw(28) << "线程池"
              << std::right
              << std::setw(10) << "平均(ms)"
              << std::setw(10) << "标准差"
              << std::setw(12) << "吞吐量(/s)"
              << std::setw(10) << "加速比"
              << std::setw(10) << "稳定性"
              << "\n";
    print_dashed();

    for (const auto& pr : scenario.pool_results)
    {
        double speedup = scenario.lock_avg > 0
            ? scenario.lock_avg / pr.avg_ms : 0;
        bool stable = is_stable(pr.times, pr.avg_ms);

        std::cout << std::left
                  << std::setw(28) << pr.pool_name
                  << std::right
                  << std::fixed << std::setprecision(2)
                  << std::setw(10) << pr.avg_ms
                  << std::setw(10) << pr.stddev_ms
                  << std::setw(12) << std::setprecision(0) << pr.throughput
                  << std::setprecision(2)
                  << std::setw(9) << speedup << "x";

        if (stable)
            std::cout << "     ✓ 稳定";
        else
            std::cout << "     ⚠ 波动大";
        std::cout << "\n";
    }

    // 如果不稳定，打印详细数据
    for (const auto& pr : scenario.pool_results)
    {
        if (!is_stable(pr.times, pr.avg_ms))
        {
            std::cout << "\n  ⚠ [" << pr.pool_name << "] 各次耗时:";
            for (size_t i = 0; i < pr.times.size(); ++i)
                std::cout << "  [" << (i+1) << "] " << std::fixed
                          << std::setprecision(2) << pr.times[i] << "ms";
            std::cout << "\n";
        }
    }
}

// 打印按线程池类型汇总的加速比趋势表
void print_summary_by_pool(
    const std::string& scenario_name,
    const std::vector<ScenarioResult>& all_results)
{
    std::cout << "\n========== " << scenario_name
              << " — 各线程数加速比汇总 ==========\n\n";

    // 收集所有唯一的池名称（保持顺序）
    std::vector<std::string> pool_names;
    for (const auto& pr : all_results[0].pool_results)
        pool_names.push_back(pr.pool_name);

    // 表头
    std::cout << std::left << std::setw(28) << "线程池";
    for (const auto& sr : all_results)
        std::cout << std::right << std::setw(8) << (std::to_string(sr.thread_count) + "线程");
    std::cout << "\n";
    print_dashed(28 + 8 * all_results.size());

    for (const auto& name : pool_names)
    {
        std::cout << std::left << std::setw(28) << name;
        for (const auto& sr : all_results)
        {
            double lock_avg = sr.lock_avg;
            for (const auto& pr : sr.pool_results)
            {
                if (pr.pool_name == name)
                {
                    double speedup = lock_avg > 0 ? lock_avg / pr.avg_ms : 0;
                    std::cout << std::right << std::fixed
                              << std::setprecision(2) << std::setw(8)
                              << speedup << "x";
                    break;
                }
            }
        }
        std::cout << "\n";
    }
}

// 打印绝对耗时趋势表
void print_absolute_time_table(
    const std::string& scenario_name,
    const std::vector<ScenarioResult>& all_results)
{
    std::cout << "\n========== " << scenario_name
              << " — 各线程数绝对耗时 (ms) ==========\n\n";

    std::vector<std::string> pool_names;
    for (const auto& pr : all_results[0].pool_results)
        pool_names.push_back(pr.pool_name);

    std::cout << std::left << std::setw(28) << "线程池";
    for (const auto& sr : all_results)
        std::cout << std::right << std::setw(8) << (std::to_string(sr.thread_count) + "线程");
    std::cout << "\n";
    print_dashed(28 + 8 * all_results.size());

    for (const auto& name : pool_names)
    {
        std::cout << std::left << std::setw(28) << name;
        for (const auto& sr : all_results)
        {
            for (const auto& pr : sr.pool_results)
            {
                if (pr.pool_name == name)
                {
                    std::cout << std::right << std::fixed
                              << std::setprecision(1) << std::setw(8)
                              << pr.avg_ms;
                    break;
                }
            }
        }
        std::cout << "\n";
    }
}

// ======================== main ========================

int main()
{
    const unsigned int hw_threads = std::thread::hardware_concurrency();
    std::cout << "============================================\n";
    std::cout << "  无锁线程池 vs 有锁线程池 性能对比测试\n";
    std::cout << "============================================\n";
    std::cout << "硬件线程数: " << hw_threads << "\n";
    std::cout << "优化级别: ";

#if defined(__OPTIMIZE__)
#if defined(__OPTIMIZE_SIZE__)
    std::cout << "-Os (大小优化)\n";
#elif defined(__clang__)
    std::cout << "-O (clang)\n";
#else
    // GCC: 检查具体优化级别宏
    #if __OPTIMIZE__ && __GNUC__
        std::cout << "-O2+ (GCC " << __GNUC__ << "."
                  << __GNUC_MINOR__ << ")\n";
    #else
        std::cout << "-O0 / 未知\n";
    #endif
#endif
#else
    std::cout << "-O0 (无优化)\n";
#endif

    std::cout << "迭代次数: 5 (每组测试取平均)\n";
    std::cout << "预热次数: 2\n";
    std::cout << "\n";

    // ======================== 测试参数 ========================

    // 线程数列表
    const std::vector<size_t> THREAD_COUNTS =
        {2, 4, 6, 8, 12, 16, 24, 32};

    // 任务量（原基准的 5 倍）
    const TestConfig SCENARIOS[] = {
        {"轻量任务(square)",  TaskType::LIGHT,  250000},  // 原 50000
        {"重量任务(fib(25))", TaskType::HEAVY,  1000},     // 原 200
        {"IO混合任务",        TaskType::IO_MIX, 10000},    // 原 2000
    };

    const int ITERATIONS = 5;
    const int WARMUP = 2;

    // ======================== 运行测试 ========================

    for (const auto& scenario_config : SCENARIOS)
    {
        print_separator();
        std::cout << "\n▶ 开始测试: " << scenario_config.name
                  << " (" << scenario_config.count << " 个任务)\n";

        std::vector<ScenarioResult> scenario_results;

        for (size_t nt : THREAD_COUNTS)
        {
            std::cout << "  测试 " << nt << " 线程...";
            std::cout.flush();

            ScenarioResult sr = test_scenario(
                scenario_config, nt, ITERATIONS, WARMUP);
            scenario_results.push_back(std::move(sr));

            std::cout << " 完成\n";
        }

        // ---- 输出 ----
        std::cout << "\n";
        print_separator();
        std::cout << "测试结果: " << scenario_config.name << "\n";
        print_separator();

        // 每个线程数的详细结果
        for (const auto& sr : scenario_results)
            print_scenario_table(sr);

        // 加速比汇总
        print_summary_by_pool(scenario_config.name, scenario_results);

        // 绝对耗时汇总
        print_absolute_time_table(scenario_config.name, scenario_results);

        std::cout << "\n";
    }

    // ======================== 最终结论 ========================

    print_separator();
    std::cout << "\n测试完成。\n";
    std::cout << "注：环形队列容量 = " << RING_CAPACITY
              << "（2^" << static_cast<int>(std::log2(RING_CAPACITY)) << "）\n";

    return 0;
}
