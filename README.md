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
├── lock_free_variant_thread_pool.hpp            # std::variant 线程池（packaged_task）
├── lock_free_strict_variant_thread_pool.hpp     # std::variant 线程池（自实现 task/future）
└── lock_free_thread_pool.hpp                    # 汇总头（包含以上四种）
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
> 支持批量操作：`enqueue_batch(T*, size_t)` / `dequeue_batch(T*, size_t)` 原子地预留连续槽位，返回实际成功数量（≤ 请求数），不会丢失任务。

## 线程池变体

四种变体均基于 CRTP 基类，零虚函数开销，均可通过模板参数替换底层队列。

### 1. DynamicThreadPool

基于 `std::function<void()>`，最通用，支持任意可调用对象。

```cpp
thread_pool::DynamicThreadPool<> pool(4);

auto f1 = pool.submit([]{ return 42; });
auto f2 = pool.submit([](int x){ return x * x; }, 5);

// 向后兼容别名（无模板参数，仅使用默认队列）
thread_pool::LockFreeThreadPool pool2(2);
```

#### 模板参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `QueueType` | `LockFreeQueue<std::function<void()>>` | 底层无锁队列类型 |
| `BatchMode` | `BatchMode::Disabled` | 批量入队开关（启用后**多生产者不安全**） |
| `AffinityMode` | `AffinityMode::Disabled` | CPU 亲和性绑定开关 |

> ⚠️ `BatchMode::Enabled` 使用非原子的成员缓冲区累积任务，仅当**单线程调用 submit** 时安全。多生产者场景须保持 `Disabled`。

分配器类型从 `QueueType::allocator_type` 推导（同 `std::priority_queue` 设计）：

```cpp
// 直接指定队列 + 分配器
using MyQueue = lock_free_container::LockFreeQueue<
    std::function<void()>, MyAllocator>;
thread_pool::DynamicThreadPool<MyQueue> pool(4, my_allocator);

// 启用 CPU 亲和性
thread_pool::DynamicThreadPool<MyQueue,
    thread_pool::BatchMode::Enabled,
    thread_pool::AffinityMode::Enabled> pool(4, my_allocator);
```

### 2. DynamicMoveOnlyThreadPool（C++23）

基于 `std::move_only_function<void()>`，支持捕获 `std::unique_ptr` 等 move-only 对象。

```cpp
thread_pool::DynamicMoveOnlyThreadPool<> pool(4);

auto ptr = std::make_unique<int>(42);
auto fut = pool.submit([p = std::move(ptr)] { return *p; });

// 向后兼容别名（无模板参数，仅使用默认队列）
thread_pool::LockFreeMoveOnlyThreadPool pool2(2);
```

#### 模板参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `QueueType` | `LockFreeQueue<std::move_only_function<void()>>` | 底层无锁队列类型 |
| `BatchMode` | `BatchMode::Disabled` | 批量入队开关（启用后**多生产者不安全**） |
| `AffinityMode` | `AffinityMode::Disabled` | CPU 亲和性绑定开关 |

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
| `VariantType` | — | 任务 variant 类型，须包含对应返回类型的 `packaged_task` 或 `std::function<void()>` 兜底 |
| `QueueType` | `LockFreeQueue<VariantType>` | 底层无锁队列类型 |
| `BatchMode` | `BatchMode::Disabled` | 批量入队开关（启用后**多生产者不安全**） |
| `AffinityMode` | `AffinityMode::Disabled` | CPU 亲和性绑定开关 |

### 4. StrictVariantThreadPool

自实现 `StaticPromise` / `StaticFuture` / `StaticPackagedTask` / `StaticFunction`，不依赖 `std::packaged_task` / `std::future` / `std::function`。`std::visit` 编译期分发，不包含保底类型——返回类型必须精确匹配 variant 中的 `StaticPackagedTask<T()>`，否则编译失败。兼容 `-fno-rtti -fno-exceptions`。轻量任务场景（2-8 线程）性能可达 variant 的 **2.1×**。

```cpp
using StrictVariant = std::variant<
    thread_pool::StaticPackagedTask<void>,
    thread_pool::StaticPackagedTask<int>,
    thread_pool::StaticPackagedTask<std::string>
>;

thread_pool::StrictVariantThreadPool<StrictVariant> pool(4);

// 返回 int → 匹配 StaticPackagedTask<int()> ✓
auto fut = pool.submit([] { return 42; });
std::cout << fut.get();  // 42

// 返回 double → 无匹配类型 → 编译错误 ✗
// pool.submit([] { return 3.14; });
```

#### 模板参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `VariantType` | — | 任务 variant 类型，须包含对应返回类型的 `StaticPackagedTask<T()>` |
| `QueueType` | `LockFreeQueue<VariantType>` | 底层无锁队列类型 |
| `BatchMode` | `BatchMode::Disabled` | 批量入队开关（启用后**多生产者不安全**） |
| `AffinityMode` | `AffinityMode::Disabled` | CPU 亲和性绑定开关 |

> 与 VariantThreadPool 的核心区别：自实现 task/future/function 替代 `std::packaged_task`/
> `std::future`/`std::function`；不保留保底类型；零虚表、零 RTTI、零异常依赖；
> 轻量任务低线程数性能更优。

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

支持三种构造方式，所有线程池变体通用：

```cpp
// 1. 通过分配器构造（最常用）
DynamicThreadPool(
    std::size_t num_threads = std::thread::hardware_concurrency(),
    const allocator_type& alloc = allocator_type(),
    std::vector<unsigned int> core_ids = {}  // 仅 AffinityMode::Enabled 时生效
);

// 2. 通过队列拷贝构造（使用预配置的队列）
DynamicThreadPool(
    std::size_t num_threads,
    const QueueType& queue,
    std::vector<unsigned int> core_ids = {}
);

// 3. 通过队列移动构造（转移队列所有权）
DynamicThreadPool(
    std::size_t num_threads,
    QueueType&& queue,
    std::vector<unsigned int> core_ids = {}
);
```

| 参数 | 说明 |
|------|------|
| `num_threads` | 工作线程数（默认 `hardware_concurrency()`） |
| `alloc` | 分配器，同 `std::priority_queue` 约定，同时用于队列节点和 `packaged_task` |
| `queue` | 预配置的队列实例（如指定容量的环形队列或自定义分配器） |
| `core_ids` | CPU 亲和性核心列表（空 = 自动分配 0,1,2,…；仅 `AffinityMode::Enabled` 时生效） |

```cpp
// 示例：环形队列 + 移动构造 + 手动指定亲和核心
using RingQ = lock_free_container::LockFreeRingQueue<
    std::function<void()>, 4096>;

RingQ queue;  // 预配置队列
thread_pool::DynamicThreadPool<RingQ,
    thread_pool::BatchMode::Enabled,
    thread_pool::AffinityMode::Enabled> pool(
    4, std::move(queue), {0, 2, 4, 6});  // 绑定到核心 0,2,4,6
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

# 性能基准测试（C++23，-O3 推荐）
g++ -std=c++23 -O3 -pthread -Ipool/include \
  pool/test/test_multi_producer.cpp -o test_mp -latomic
```

> 某些 x86-64 平台需 `-latomic`（128 位原子操作）。

## 性能

以下数据基于 16 物理核心（32 逻辑线程，WSL 限 24），GCC 15.2，-O3：

### 基准对比（12 工作线程）

| 场景 | 有锁线程池 | function+环形 | move_only+环形 | variant+环形 | strict+环形 | 最佳加速比 |
|------|-----------|--------------|----------------|-------------|------------|-----------|
| 轻量任务 (250000) | 3382 ms | 403 ms | 309 ms | 36 ms | **16 ms** | **211×** |
| 重量任务 (1000) | 13 ms | ~5.5 ms | ~5.5 ms | ~5.5 ms | ~5.5 ms | **2.4×** |
| IO 混合 (10000) | 93 ms | 92 ms | 92 ms | 92 ms | 91 ms | 1.02× |

> 测试程序：`pool/test/compare_with_mutex_pool.cpp`，编译需 `-std=c++23 -O3`。
> 链表队列版本在轻量任务中耗时约为环形队列的 1.6–2×（每次入队需堆分配）。
> strict 在 2 线程时可达 variant 的 **2.1×** 速度——StaticFunction 的类型擦除
> 比 `std::packaged_task` 更轻量（无 shared-state 开销），且 SBO 避免堆分配。

### 各线程数扩展性（variant vs strict，环形队列，轻量任务 ms）

| 实现 | 2线程 | 4线程 | 6线程 | 8线程 | 12线程 | 16线程 | 24线程 | 32线程 |
|------|------|------|------|------|-------|-------|-------|-------|
| variant+环形 | 35 | 25 | 29 | 44 | 53 | 39 | 56 | 86 |
| **strict+环形** | **16** | **22** | **23** | **30** | 55 | 51 | 68 | 82 |

> strict 在 2–8 线程（轻度竞争）时快 1.3–2.1×；12+ 线程后两者收敛，
> 瓶颈从任务构造转移到队列 CAS 竞争。重量/IO 任务各实现持平。

### 性能要点

- **轻量任务**：variant + 环形队列组合消除类型擦除且无堆分配，4 线程 94× 于有锁池。超过 6 线程后退化——主线程提交 250K 任务是硬瓶颈（~77ns/task），后续优化方向为 Work Stealing。
- **重量任务**：任务执行（fib(25) 约 70μs）远大于调度开销，无锁池 24 线程达最佳；有锁池因互斥锁上下文切换在 12 线程后退化（效率从 100% 跌至 19%）。
- **IO 混合**：瓶颈在 sleep(1ms)，实际 CPU 占用 ≈ 线程数的 10%，各池在 12 线程以下无差异，高线程数时无锁池因锁竞争更小逐渐领先。
- **CAS 渐进退避**：24 线程场景下退避策略减少无效 CAS 重试，相比优化前提速约 23%（66ms → 50ms）。

### 优化策略实测

以下测试基于 StrictVariantThreadPool + RingQueue(65536)，GCC 15.2 -O3，10000 个 `square()` 轻量任务，2 轮取平均。
测试程序：`pool/test/test_multi_producer.cpp`、`test_batch_submit.cpp`、`test_affinity.cpp`。

#### 多生产者提交

单生产者是轻量任务场景的硬瓶颈——主线程串行入队速率赶不上多个工作线程的消费速度。多线程同时 `submit` 可显著缓解：

| 配置 | 4 线程 | 8 线程 |
|------|--------|--------|
| 1 生产者 | 3.8 ms | 11.6 ms |
| 2 生产者 | 1.7 ms (**2.3×**) | 2.8 ms (**4.1×**) |
| 4 生产者 | 1.8 ms (**2.2×**) | 2.8 ms (**4.1×**) |

> 8 线程下仅 2 生产者即获 4.1× 加速，打破了生产者速率天花板。4 线程下 2→4 生产者无进一步收益——此时瓶颈已从生产者转移到消费者 `dequeue_pos_` CAS 竞争。

#### 批量入队（BatchMode）

单生产者场景下，`BatchMode::Enabled` 将任务聚合为 64 个一批再入队，减少生产者侧 CAS 次数：

| BatchMode | 4 线程 | 8 线程 |
|-----------|--------|--------|
| Disabled | 9.9 ms | 12.3 ms |
| Enabled | 3.0 ms (**3.3×**) | 5.3 ms (**2.3×**) |

> `wait_all()` 已内部调用 `flush_batch()` 确保缓冲区任务入队，无需手动处理。⚠️ 多线程 `submit` 不兼容 `BatchMode::Enabled`（缓冲区非原子操作）。

#### CPU 亲和性

| 配置 | 4 线程 | 8 线程 |
|------|--------|--------|
| 基准（全关） | 9.6 ms | 11.9 ms |
| 仅 Affinity | 4.5 ms (**2.1×**) | 14.8 ms (0.8×) |
| Batch + Affinity | 3.1 ms (**3.1×**) | 6.6 ms (**1.8×**) |

> 亲和性在低线程数效果显著，但 8 线程下单独开启反而倒退——固定核心绑定限制了 OS 调度器处理单生产者-多消费者负载不均的能力。`BatchMode::Enabled` + `AffinityMode::Enabled` 组合在两种场景下均优于基准。

#### 策略对比：多生产者 vs 批量入队

以下测试将四种优化策略放在同一条件下横向对比（strict+环形，10000 轻量任务，2 轮平均）。
测试程序：`pool/test/test_mp_vs_batch.cpp`。

| 策略 | 4 线程 | 8 线程 |
|------|--------|--------|
| 基准（1 生产者，Batch×） | 2.8 ms | 11.0 ms |
| 批量入队（1 生产者，Batch✓） | 2.5 ms (**1.1×**) | 5.2 ms (**2.1×**) |
| 多生产者（2 生产者，Batch×） | 1.6 ms (**1.7×**) | 2.8 ms (**3.9×**) |
| 多生产者（4 生产者，Batch×） | 1.9 ms (**1.5×**) | 2.6 ms (**4.3×**) |

> **结论**：多生产者是轻量任务场景下最有效的优化——8 线程时 2 生产者获 3.9× 加速，超过批量入队的 2.1×。4 线程时批量入队收益有限（1.1×），因生产者侧 CAS 此时并非瓶颈。推荐方案：多生产者提交（`BatchMode::Disabled`）> 批量入队（`BatchMode::Enabled`，仅单生产者安全）。

### 编译期配置

线程池支持两个模板开关，位于 `thread_pool` 命名空间：

| 选项 | 枚举值 | 说明 |
|------|--------|------|
| `BatchMode` | `Disabled`（默认）/ `Enabled` | 批次入队减少 CAS（⚠️ Enabled 仅单生产者安全） |
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

底层自动生效的优化：PAUSE→yield→sleep 三级 CAS 退避、每线程独立统计槽位消除 `completed_count()` 缓存弹跳、`submit()` 多生产者安全（`BatchMode::Disabled` 时）。

## 编译器要求

- C++20（DynamicThreadPool、VariantThreadPool）
- C++23（DynamicMoveOnlyThreadPool，需 `<move_only_function>`）
- 支持 GCC 15+、Clang 21+、MSVC 2022+

## 许可证

MIT License — 详见 [LICENSE](LICENSE)。
