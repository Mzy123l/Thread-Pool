#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <cmath>
#include <thread>
#include <variant>
#include <iomanip>

#include "../include/lock_free_thread_pool.hpp"
#include "../include/lock_free_ring_queue.hpp"

using Clock = std::chrono::high_resolution_clock;
using SV = std::variant<thread_pool::StaticPackagedTask<void>,
                        thread_pool::StaticPackagedTask<int>,
                        thread_pool::StaticPackagedTask<long long>>;
using RQ = lock_free_container::LockFreeRingQueue<SV, 65536>;
using Pool = thread_pool::StrictVariantThreadPool<
    SV, RQ, thread_pool::BatchMode::Disabled, thread_pool::AffinityMode::Disabled>;
using PoolBatch = thread_pool::StrictVariantThreadPool<
    SV, RQ, thread_pool::BatchMode::Enabled, thread_pool::AffinityMode::Disabled>;

constexpr int ITER = 2;
constexpr size_t TASKS = 10000;

long long square(long long x) { volatile long long y = x * x; return y; }

// 单生产者
template <typename P>
double run_single(int nw)
{
    P pool(static_cast<size_t>(nw));
    auto t0 = Clock::now();
    for (size_t i = 0; i < TASKS; ++i)
        pool.submit([i]() { square(i % 1024); });
    pool.wait_all();
    auto t1 = Clock::now();
    pool.shutdown();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// 多生产者（BatchMode::Disabled 确保安全）
double run_multi(int nw, int np)
{
    Pool pool(static_cast<size_t>(nw));
    auto t0 = Clock::now();
    std::vector<std::thread> prods;
    for (int p = 0; p < np; ++p)
    {
        prods.emplace_back([&pool, p, np]()
        {
            size_t beg = static_cast<size_t>(p) * TASKS / static_cast<size_t>(np);
            size_t end = static_cast<size_t>(p + 1) * TASKS / static_cast<size_t>(np);
            for (size_t i = beg; i < end; ++i)
                pool.submit([i]() { square(i % 1024); });
        });
    }
    for (auto& t : prods) t.join();
    pool.wait_all();
    auto t1 = Clock::now();
    pool.shutdown();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main()
{
    std::cout << "多生产者 vs 批量入队对比  GCC -O3  任务:" << TASKS
              << "  迭代:" << ITER << std::endl << std::endl;

    struct R { std::string label; double ms4; double ms8; };
    std::vector<R> results;

    // 基准: 1P + Batch×
    {
        std::vector<double> t4, t8;
        for (int r = 0; r < ITER; ++r) t4.push_back(run_single<Pool>(4));
        for (int r = 0; r < ITER; ++r) t8.push_back(run_single<Pool>(8));
        double a4 = std::accumulate(t4.begin(), t4.end(), 0.0)/t4.size();
        double a8 = std::accumulate(t8.begin(), t8.end(), 0.0)/t8.size();
        results.push_back({"基准 (1P, Batch×)", a4, a8});
        std::cout << "基准 (1P, Batch×)    4线程=" << std::fixed << std::setprecision(2)
                  << a4 << "ms  8线程=" << a8 << "ms" << std::endl;
    }

    // 批量入队: 1P + Batch✓
    {
        std::vector<double> t4, t8;
        for (int r = 0; r < ITER; ++r) t4.push_back(run_single<PoolBatch>(4));
        for (int r = 0; r < ITER; ++r) t8.push_back(run_single<PoolBatch>(8));
        double a4 = std::accumulate(t4.begin(), t4.end(), 0.0)/t4.size();
        double a8 = std::accumulate(t8.begin(), t8.end(), 0.0)/t8.size();
        results.push_back({"批量入队 (1P, Batch✓)", a4, a8});
        std::cout << "批量入队 (1P, Batch✓) 4线程=" << a4 << "ms  8线程=" << a8 << "ms" << std::endl;
    }

    // 多生产者: 2P + Batch×
    {
        std::vector<double> t4, t8;
        for (int r = 0; r < ITER; ++r) t4.push_back(run_multi(4, 2));
        for (int r = 0; r < ITER; ++r) t8.push_back(run_multi(8, 2));
        double a4 = std::accumulate(t4.begin(), t4.end(), 0.0)/t4.size();
        double a8 = std::accumulate(t8.begin(), t8.end(), 0.0)/t8.size();
        results.push_back({"多生产者 (2P, Batch×)", a4, a8});
        std::cout << "多生产者 (2P, Batch×) 4线程=" << a4 << "ms  8线程=" << a8 << "ms" << std::endl;
    }

    // 多生产者: 4P + Batch×
    {
        std::vector<double> t4, t8;
        for (int r = 0; r < ITER; ++r) t4.push_back(run_multi(4, 4));
        for (int r = 0; r < ITER; ++r) t8.push_back(run_multi(8, 4));
        double a4 = std::accumulate(t4.begin(), t4.end(), 0.0)/t4.size();
        double a8 = std::accumulate(t8.begin(), t8.end(), 0.0)/t8.size();
        results.push_back({"多生产者 (4P, Batch×)", a4, a8});
        std::cout << "多生产者 (4P, Batch×) 4线程=" << a4 << "ms  8线程=" << a8 << "ms" << std::endl;
    }

    // 汇总
    std::cout << "\n  汇总 — 加速比 (vs 基准):\n";
    std::cout << std::string(60, '-') << std::endl;
    std::cout << std::left << std::setw(24) << "策略"
              << std::right << std::setw(16) << "4 线程"
              << std::setw(16) << "8 线程" << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    double base4 = results[0].ms4, base8 = results[0].ms8;
    for (const auto& r : results)
    {
        std::cout << std::left << std::setw(24) << r.label
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(10) << r.ms4 << "ms "
                  << std::setw(5) << (base4 / r.ms4) << "x"
                  << std::setw(10) << r.ms8 << "ms "
                  << std::setw(5) << (base8 / r.ms8) << "x"
                  << std::endl;
    }
    std::cout << std::endl << "完成" << std::endl;
    return 0;
}
