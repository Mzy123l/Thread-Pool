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

| 模板参数 | 默认值 | 说明 |
|----------|--------|------|
| `T` | — | 元素类型（须默认可构造） |
| `Allocator` | `DefaultAllocator<T>` | 分配器，暴露为 `allocator_type` |

> `T` 必须默认可构造（内部哨兵节点需要）。  
> 分配器副本通过 `get_allocator()` 获取。

### LockFreeRingQueue（环形，有界）

Vyukov MPMC 算法，序列号法防 ABA，无需 128-bit CAS。缓存行对齐消除 false sharing。

```cpp
lock_free_container::LockFreeRingQueue<int, 1024> queue;  // 容量须为 2 的幂
queue.enqueue(42);          // 满时返回 false
int v; queue.dequeue(v);    // 空时返回 false
```

| 模板参数 | 默认值 | 说明 |
|----------|--------|------|
| `T` | — | 元素类型 |
| `Capacity` | — | 容量（须为 2 的幂） |
| `Allocator` | `DefaultAllocator<T>` | 分配器，暴露为 `allocator_type` |

> `T` 无需默认可构造。  
> 分配器副本通过 `get_allocator()` 获取。

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

#### 模板参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `QueueType` | `LockFreeQueue<std::function<void()>>` | 底层无锁队列类型，分配器从 `QueueType::allocator_type` 推导 |

分配器类型从队列类型推导（同 `std::priority_queue` 设计）。
若需自定义分配器，将其嵌入队列类型：

```cpp
// 方式一：通过向后兼容别名
thread_pool::LockFreeThreadPool<MyAllocator> pool(4);

// 方式二：直接指定队列类型
using MyQueue = lock_free_container::LockFreeQueue<
    std::function<void()>, MyAllocator>;
thread_pool::DynamicThreadPool<MyQueue> pool(4, my_allocator);
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

#### 模板参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `QueueType` | `LockFreeQueue<std::move_only_function<void()>>` | 底层无锁队列类型，分配器从 `QueueType::allocator_type` 推导 |

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

#### 模板参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `VariantType` | 无 | 任务 variant 类型，须包含对应返回类型的 `packaged_task` 或 `std::function<void()>` 兜底 |
| `QueueType` | `LockFreeQueue<VariantType>` | 底层无锁队列类型，分配器从 `QueueType::allocator_type` 推导 |

## 替换队列与分配器

所有线程池均支持通过模板参数指定底层队列实现，分配器可嵌入队列类型：

```cpp
// 使用环形队列
using RingQ = lock_free_container::LockFreeRingQueue<
    std::function<void()>, 256>;

thread_pool::DynamicThreadPool<RingQ> pool(4);
```

使用自定义分配器（需在队列类型中指定）：

```cpp
// 自定义分配器
template <typename T>
struct MyAlloc : std::allocator<T> { /* ... */ };

// 嵌入队列类型
using MyQueue = lock_free_container::LockFreeQueue<
    std::function<void()>, MyAlloc<std::function<void()>>>;

// 传递分配器实例
MyAlloc<std::function<void()>> alloc;
thread_pool::DynamicThreadPool<MyQueue> pool(4, alloc);
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
| `get_allocator()` | 获取分配器副本（同 `std::priority_queue` 约定） |

### 构造参数

```cpp
// num_threads: 工作线程数（默认硬件并发数）
// alloc:       分配器实例，透传给底层队列（默认构造）
DynamicThreadPool(
    std::size_t num_threads = std::thread::hardware_concurrency(),
    const allocator_type& alloc = allocator_type()
);
```

分配器同时用于：
- 底层队列的节点和数据内存分配
- `submit()` 中 `packaged_task` 的内存分配（经 `allocator_traits::rebind`）

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

以下数据基于 16 物理核心（32 逻辑线程，WSL 限 24），GCC 15.2，-O3：

### 基准对比（12 工作线程）

| 场景 | 有锁线程池 | function+环形 | move_only+环形 | variant+环形 | 最佳加速比 |
|------|-----------|--------------|----------------|-------------|-----------|
| 轻量任务 (250000) | 3382 ms | 403 ms | 309 ms | 36 ms | **94×** |
| 重量任务 (1000) | 13 ms | 5.4 ms | 5.5 ms | 5.6 ms | **2.5×** |
| IO 混合 (10000) | 93 ms | 92 ms | 92 ms | 92 ms | 1.01× |

> 测试程序：`pool/test/compare_with_mutex_pool.cpp`，编译需 `-std=c++23 -O3`。
> 链表队列版本在轻量任务中耗时约为环形队列的 1.6–2×（每次入队需堆分配）。

### 各线程数扩展性（variant + 环形队列）

| 场景 | 2线程 | 4线程 | 6线程 | 8线程 | 12线程 | 16线程 | 24线程 | 32线程 |
|------|------|------|------|------|-------|-------|-------|-------|
| 轻量任务 (ms) | 36 | **19** | 20 | 28 | 38 | 40 | **50** | 74 |
| 重量任务 (ms) | 33 | 16 | 11 | 8.1 | 5.4 | 5.0 | **4.3** | 5.0 |
| IO混合 (ms) | 547 | 274 | 183 | 137 | 92 | 69 | 46 | **35** |

> 轻量任务最佳在 4–6 线程（主线程单生产者瓶颈），重量任务 24 线程最优，
> IO 混合遵循 1/N 线性扩展。

### 性能要点

- **轻量任务**：variant + 环形队列组合消除类型擦除且无堆分配，4 线程 94× 于有锁池。超过 6 线程后退化——主线程提交 250K 任务是硬瓶颈（~77ns/task），后续优化方向为 Work Stealing。
- **重量任务**：任务执行（fib(25) 约 70μs）远大于调度开销，无锁池 24 线程达最佳；有锁池因互斥锁上下文切换在 12 线程后退化（效率从 100% 跌至 19%）。
- **IO 混合**：瓶颈在 sleep(1ms)，实际 CPU 占用 ≈ 线程数的 10%，各池在 12 线程以下无差异，高线程数时无锁池因锁竞争更小逐渐领先。
- **CAS 渐进退避**：24 线程场景下退避策略减少无效 CAS 重试，相比优化前提速约 23%（66ms → 50ms）。

### 编译期配置

线程池支持两个模板开关，位于 `thread_pool` 命名空间：

| 选项 | 枚举值 | 说明 |
|------|--------|------|
| `BatchMode` | `Disabled`（默认）/ `Enabled` | 将任务聚合为批次一次性入队，减少 CAS 次数 |
| `AffinityMode` | `Disabled`（默认）/ `Enabled` | Linux 下 `pthread_setaffinity_np` 绑定线程到不同核心 |

```cpp
using MyVariant = std::variant<
    std::packaged_task<void()>,
    std::packaged_task<int()>,
    std::function<void()>
>;
using MyQ = lock_free_container::LockFreeRingQueue<MyVariant, 65536>;

thread_pool::VariantThreadPool<MyVariant, MyQ,
    thread_pool::BatchMode::Enabled,
    thread_pool::AffinityMode::Enabled> pool(8);
```

底层自动生效的优化：PAUSE→yield→sleep 三级 CAS 退避、每线程独立统计槽位消除 `completed_count()` 缓存弹跳、`submit()` 多生产者安全。

- **轻量任务**：variant + 环形队列组合因编译期分发消除类型擦除、且环形队列无内存分配，性能最优（4线程时 94× 于有锁池）。但受主线程单生产者瓶颈限制，最佳线程数 4–6，超过后 CAS 竞争增加导致退化。环形队列比链表队列快 1.6–2×（省去每次 `new`/`delete`）。
- **重量任务**：任务执行时间（fib(25) 约 70μs）远大于调度开销，无锁池在 16–24 线程达最佳，有锁池因互斥锁争用在 8 线程后退化。
- **IO 混合**：12 线程以下瓶颈在 sleep，各池无差异；16 线程起无锁池领先（sleep(1ms) 占比 10% 的任务消化更快），32 线程加速比 ~3.9×。
- **false sharing**：基类原子计数器（`active_tasks_` 等）经 `alignas(64)` 缓存行隔离后，低线程时性能随线程数线性扩展，消除了"线程越多越慢"的反常现象。

## 编译器要求

- C++20（DynamicThreadPool、VariantThreadPool）
- C++23（DynamicMoveOnlyThreadPool，需 `<move_only_function>`）
- 支持 GCC 15+、Clang 21+、MSVC 2022+

## 许可证

MIT License — 详见 [LICENSE](LICENSE)。
