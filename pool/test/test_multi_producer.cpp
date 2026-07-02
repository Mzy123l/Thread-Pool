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

constexpr int ITER = 2;
constexpr size_t TASKS = 10000;

long long square(long long x) { volatile long long y = x * x; return y; }

int main()
{
    std::cout << "多生产者测试  GCC -O3  任务:" << TASKS
              << "  迭代:" << ITER << std::endl << std::endl;

    for (int nw : {4, 8})
    {
        for (int np : {1, 2, 4})
        {
            std::vector<double> times;
            for (int r = 0; r < ITER; ++r)
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
                times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                pool.shutdown();
            }
            double a = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
            std::cout << nw << "线程×" << np << "生产者 → "
                      << std::fixed << std::setprecision(2) << a << " ms";
            if (times.size() > 1)
            {
                double s = 0;
                for (double t : times) s += (t - a) * (t - a);
                std::cout << " σ=" << std::sqrt(s / (times.size() - 1));
            }
            std::cout << "  [" << times[0] << ", " << (times.size()>1?times[1]:0) << "]"
                      << std::endl;
        }
    }
    std::cout << std::endl << "完成" << std::endl;
    return 0;
}
