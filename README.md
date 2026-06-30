# Thread-Pool

基于 C++20 的无锁线程池库，header-only，无外部依赖。

## 目录结构

```text
pool/include/
├── lock_free_utility.hpp                 # 编译器内建宏与编译期工具
├── lock_free_queue.hpp                   # Michael-Scott 链表无锁队列
├── lock_free_ring_queue.hpp              # Vyukov 有界环形无锁队列
├── lock_free_thread_pool_base.hpp        # CRTP 基类
├── lock_free_dynamic_thread_pool.hpp     # std::function 线程池
├── lock_free_move_only_thread_pool.hpp   # std::move_only_function 线程池 (C++23)
├── lock_free_variant_thread_pool.hpp     # std::variant 线程池
└── lock_free_thread_pool.hpp             # 汇总头（包含以上三种）
```

## 快速开始

```cpp
#include "lock_free_thread_pool.hpp"

// 4 线程池，自动推导硬件并发数
thread_pool::DynamicThreadPool<> pool(4);

// 提交任务，返回 std::future
auto result = pool.submit([](int a, int b) { return a + b; }, 10, 20);
std::cout << result.get();  // 30
```

## 无锁队列

### LockFreeQueue（链表，无界）

Michael-Scott 算法，双字 CAS 防 ABA，`thread_local` 延迟释放防悬垂指针。

```cpp
lock_free_container::LockFreeQueue<int> queue;
queue.enqueue(42);
int v; queue.dequeue(v);  // v = 42
```

> **注意**：`T` 必须默认可构造（内部哨兵节点需要）。

### LockFreeRingQueue（环形，有界）

Vyukov MPMC 算法，序列号法防 ABA，无需 128-bit CAS。缓存行对齐消除 false sharing。

```cpp
lock_free_container::LockFreeRingQueue<int, 1024> queue;  // 容量须为 2 的幂
queue.enqueue(42);          // 满时返回 false
int v; queue.dequeue(v);    // 空时返回 false
```

> `T` 无需默认可构造。

## 线程池变体

三种变体均基于 CRTP 基类，零虚函数开销，均可通过模板参数替换底层队列。

### 1. DynamicThreadPool

基于 `std::function<void()>`，最通用，支持任意可调用对象。

```cpp
thread_pool::DynamicThreadPool<> pool(4);

auto f1 = pool.submit([]{ return 42; });
auto f2 = pool.submit([](int x){ return x * x; }, 5);

// 向后兼容别名
thread_pool::LockFreeThreadPool<> pool2(2);
```

### 2. DynamicMoveOnlyThreadPool（C++23）

基于 `std::move_only_function<void()>`，支持捕获 `std::unique_ptr` 等 move-only 对象。

```cpp
thread_pool::DynamicMoveOnlyThreadPool<> pool(4);

auto ptr = std::make_unique<int>(42);
auto fut = pool.submit([p = std::move(ptr)] { return *p; });

// 向后兼容别名
thread_pool::LockFreeMoveOnlyThreadPool<> pool2(2);
```

### 3. VariantThreadPool

基于 `std::variant<PackagedTasks...>` 消除运行时类型擦除，`std::visit` 编译期跳转表替代虚函数分发。

```cpp
using TaskVariant = std::variant<
    std::packaged_task<void()>,
    std::packaged_task<int()>,
    std::packaged_task<std::string()>,
    std::function<void()>         // 兜底类型
>;

thread_pool::VariantThreadPool<TaskVariant> pool(4);
auto fut = pool.submit([]{ return 42; });  // 自动走 packaged_task<int()>
```

## 替换队列类型

所有线程池均支持通过模板参数指定底层队列实现：

```cpp
// 使用环形队列
using RingQ = lock_free_container::LockFreeRingQueue<
    std::function<void()>, 256>;

thread_pool::DynamicThreadPool<RingQ> pool(4);
```

## API 参考

### 公共接口（所有线程池）

| 方法 | 说明 |
|------|------|
| `submit(Func&&, Args&&...)` | 提交任务，返回 `std::future<ReturnType>` |
| `wait_all()` | 等待所有活跃任务完成 |
| `shutdown()` | 优雅关闭：等待进行中任务完成，不再接受新任务 |
| `shutdown_now()` | 立即关闭：丢弃未执行任务，等待进行中任务完成 |
| `active_count()` | 当前活跃（排队中 + 执行中）任务数 |
| `total_count()` | 历史提交总数 |
| `completed_count()` | 已完成任务数 |
| `thread_count()` | 工作线程数 |

### 构造参数

```cpp
// num_threads: 工作线程数（默认硬件并发数）
// queue:       任务队列实例（默认构造）
DynamicThreadPool(
    std::size_t num_threads = std::thread::hardware_concurrency(),
    QueueType queue = QueueType()
);
```

## 构建与测试

### CMake（推荐）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
ctest --output-on-failure
```

> GoogleTest 通过 FetchContent 自动下载（v1.15.2）。

### 手动编译

```bash
# 基础测试（C++20）
g++ -std=c++20 -O2 -pthread pool/test/test_queue.cpp -o test_queue -latomic

# move_only 测试（C++23）
g++ -std=c++23 -O2 -pthread pool/test/test_move_only.cpp -o test_move_only -latomic
```

> 某些 x86-64 平台需 `-latomic`（128 位原子操作）。

## 性能

以下数据基于 12 核 CPU，GCC 15.2，-O2：

| 场景 | 有锁线程池 | 无锁 function | 无锁 move_only | 加速比 |
|------|-----------|--------------|---------------|--------|
| 轻量任务 (50000) | 493 ms | 282 ms | 229 ms | **2.15×** |
| 重量任务 (200) | 33 ms | 14 ms | 18 ms | **2.33×** |
| IO 混合 (2000) | 20 ms | 21 ms | 19 ms | 1.08× |

## 编译器要求

- C++20（DynamicThreadPool、VariantThreadPool）
- C++23（DynamicMoveOnlyThreadPool，需 `<move_only_function>`）
- 支持 GCC 15+、Clang 21+、MSVC 2022+

## 许可证

MIT License — 详见 [LICENSE](LICENSE)。
