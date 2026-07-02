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
using PoolOff = thread_pool::StrictVariantThreadPool<
    SV, RQ, thread_pool::BatchMode::Disabled, thread_pool::AffinityMode::Disabled>;
using PoolOn = thread_pool::StrictVariantThreadPool<
    SV, RQ, thread_pool::BatchMode::Enabled, thread_pool::AffinityMode::Disabled>;

constexpr int ITER = 2;
constexpr size_t TASKS = 10000;

long long square(long long x) { volatile long long y = x * x; return y; }

// 用作用域析构来确保 BatchMode 的批量缓冲区被 flush（析构函数保证 flush→shutdown 顺序）
template <typename Pool>
double run_once(int nw)
{
    auto t0 = Clock::now();
    {
        Pool pool(static_cast<size_t>(nw));
        for (size_t i = 0; i < TASKS; ++i)
            pool.submit([i]() { square(i % 1024); });
        // 不能调 wait_all()（BatchMode 任务还在缓冲区），
        // 直接让析构函数：先 ensure_batch_flushed → 再 shutdown（join workers）
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main()
{
    std::cout << "批量提交测试  GCC -O3  任务:" << TASKS
              << "  迭代:" << ITER << "  (单生产者)" << std::endl
              << "  注: BatchMode::Enabled 依赖析构函数 flush→shutdown 顺序" << std::endl << std::endl;

    for (int nw : {4, 8})
    {
        // BatchMode::Disabled
        {
            std::vector<double> times;
            for (int r = 0; r < ITER; ++r)
                times.push_back(run_once<PoolOff>(nw));
            double a = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
            std::cout << "Batch× " << nw << "线程 → "
                      << std::fixed << std::setprecision(2) << a << " ms"
                      << "  [" << times[0] << ", " << times[1] << "]" << std::endl;
        }
        // BatchMode::Enabled
        {
            std::vector<double> times;
            for (int r = 0; r < ITER; ++r)
                times.push_back(run_once<PoolOn>(nw));
            double a = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
            std::cout << "Batch✓ " << nw << "线程 → "
                      << std::fixed << std::setprecision(2) << a << " ms"
                      << "  [" << times[0] << ", " << times[1] << "]" << std::endl;
        }
    }
    std::cout << std::endl << "完成" << std::endl;
    return 0;
}
