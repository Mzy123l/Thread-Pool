# Thread-Pool

基于 C++20 的无锁队列和线程池库，header-only，无外部依赖。

## 目录结构

```text
Thread-Pool/
├── README.md
├── LICENSE
├── CMakeLists.txt
├── .gitignore
└── pool/
    ├── include/
    │   ├── lock_free_utility.hpp              # 编译器内建宏与工具
    │   ├── lock_free_queue.hpp                # 链表无锁队列
    │   ├── lock_free_ring_queue.hpp           # 环形无锁队列（新增）
    │   ├── lock_free_thread_pool_base.hpp     # CRTP 基类
    │   ├── lock_free_thread_pool.hpp          # 三种线程池变体
    │   └── lock_free_move_only_thread_pool.hpp # 向后兼容头文件
    └── test/
        ├── test_queue.cpp                     # 链表队列 GTest
        ├── test_ring_queue.cpp                # 环形队列 GTest
        ├── test_thread_pool.cpp               # 线程池 TYPED_TEST
        ├── test_concurrency.cpp               # 并发压力测试
        ├── test_move_only.cpp                 # move-only 旧测试
        └── compare_with_mutex_pool.cpp        # 性能基准
```

## 功能说明

### 无锁队列

#### LockFreeQueue（链表实现）

`lock_free_container::LockFreeQueue<T>` — Michael-Scott 无锁队列：

- `enqueue(Args&&...)` / `dequeue(T&)` — 多生产者多消费者安全
- 双字 CAS（128-bit）防 ABA 问题
- `thread_local` 延迟释放防悬垂指针
- 支持自定义分配器

```cpp
lock_free_container::LockFreeQueue<int> queue;
queue.enqueue(42);
int v; queue.dequeue(v);  // v == 42
```

#### LockFreeRingQueue（环形缓冲区，新增）

`lock_free_container::LockFreeRingQueue<T, Capacity>` — Vyukov 有界 MPMC 队列：

- `Capacity` 编译期常量（须为 2 的幂）
- 序列号法防 ABA，无需 128-bit CAS
- 缓存行对齐消除 false sharing
- 无需默认构造 `T`

```cpp
lock_free_container::LockFreeRingQueue<int, 1024> queue;
queue.enqueue(42);  // 满时返回 false
int v; queue.dequeue(v);
```

### 线程池

三种变体共享 CRTP 基类，零虚函数开销。

#### DynamicThreadPool（`std::function`）

```cpp
thread_pool::DynamicThreadPool<> pool(4);  // 或 LockFreeThreadPool<>
auto fut = pool.submit([](int a, int b) { return a + b; }, 10, 20);
std::cout << fut.get();  // 30
```

#### DynamicMoveOnlyThreadPool（`std::move_only_function`，需 C++23）

```cpp
thread_pool::DynamicMoveOnlyThreadPool<> pool(4);
auto fut = pool.submit([p = std::make_unique<int>(42)] { return *p; });
```

#### VariantThreadPool（消除类型擦除，新增）

模板参数为 `std::variant<PackagedTasks...>`，以 `std::visit` 编译期跳转表替代
`std::function` 的虚函数分发：

```cpp
using V = std::variant<
    std::packaged_task<void()>,
    std::packaged_task<int()>,
    std::function<void()>>;  // 兜底

thread_pool::VariantThreadPool<V> pool(4);
auto fut = pool.submit([] { return 42; });  // 走 packaged_task<int()>
```

### 替换队列类型

所有线程池均支持通过模板参数指定底层队列：

```cpp
// 环形队列替代链表队列
using RingQ = lock_free_container::LockFreeRingQueue<
    std::function<void()>, 256>;
thread_pool::DynamicThreadPool<RingQ> pool(4);
```

## 构建与测试

### CMake（推荐）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
ctest --output-on-failure
```

GoogleTest 通过 FetchContent 自动下载（v1.15.2），无需手动安装。

C++23 相关目标（`test_move_only`、`test_compare`）在编译器不支持时自动跳过。

### 手动编译

```bash
# C++20
g++ -std=c++20 -O2 -pthread pool/test/test_queue.cpp -o test_queue -latomic

# C++23（move_only）
g++ -std=c++23 -O2 -pthread pool/test/test_move_only.cpp -o test_move_only -latomic
```

某些 x86-64 平台需 `-latomic`（128 位原子操作）。

## 注意事项

- `LockFreeQueue<T>` 需 `T` 默认可构造（哨兵节点）；`LockFreeRingQueue<T, N>` 无此限制
- 线程池析构自动调用 `shutdown()`，会等待所有任务完成
- `DynamicMoveOnlyThreadPool` 和 `LockFreeMoveOnlyThreadPool` 需 `<move_only_function>`（C++23）
- `VariantThreadPool` 的 variant 中可包含 `std::function<void()>` 作为未注册返回类型的兜底

## 许可证

MIT License — 详见 LICENSE 文件。
