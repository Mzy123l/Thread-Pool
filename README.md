# Thread-Pool

基于 C++23 的无锁队列和线程池示例项目。核心代码位于 `pool/include`。

## 目录结构

```text
Thread-Pool/
├── README.md
├── test.cpp
├── LICENSE
└── pool/
    ├── include/
    │   ├── lock_free_queue.hpp
    │   └── lock_free_thread_pool.hpp
    └── test/
        └── pool.cpp
```

## 核心架构

线程池当前拆成一个基类和三类派生池：

```cpp
lock_free::inner_lock_free::Base<FuncType, Derived, Allocator>
lock_free::DynamicPool
lock_free::DynamicMoveOnlyPool
lock_free::StaticPool
```

差异如下：

| 类型 | 队列中保存的任务类型 | 特点 |
| --- | --- | --- |
| `lock_free::DynamicPool` | `std::function<void()>` | 通用、兼容性强，但有类型擦除开销 |
| `lock_free::DynamicMoveOnlyPool` | `std::move_only_function<void()>` | 仍然类型擦除，但可以直接保存 move-only packaged task，避免 `shared_ptr` 控制块带来的原子开销 |
| `lock_free::StaticPool<FuncType>` | 用户指定的具体 `FuncType` | 无类型擦除，适合 `void(*)()` 等固定函数指针或固定 callable 类型 |

分配器参数仍然保留：

```cpp
lock_free::DynamicPool<std::allocator<std::function<void()>>> pool1;
lock_free::DynamicMoveOnlyPool<std::allocator<std::move_only_function<void()>>> pool2;
lock_free::StaticPool<void(*)(), std::allocator<void(*)()>> pool3;
```

## 使用示例

### DynamicPool

```cpp
lock_free::DynamicPool<> pool(4);

auto result = pool.submit([](int a, int b) {
    return a + b;
}, 10, 20);

std::cout << result.get() << '\n';
pool.wait_all();
pool.shutdown();
```

### DynamicMoveOnlyPool

```cpp
lock_free::DynamicMoveOnlyPool<> pool(4);

auto result = pool.submit([] {
    return 123;
});

std::cout << result.get() << '\n';
pool.wait_all();
pool.shutdown();
```

### StaticPool

```cpp
void task()
{
    // do work
}

lock_free::StaticPool<void(*)()> pool(2);
pool.submit_static(&task);
pool.wait_all();
pool.shutdown();
```

`StaticPool` 不接收任意不同类型的 lambda；它保存的是模板参数指定的精确 `FuncType`。这正是它没有任务类型擦除开销的原因。

## 编译

项目使用 `std::move_only_function`，需要 C++23：

```bash
g++ -std=c++23 -O2 -pthread test.cpp -o test -latomic
./test
```

Windows PowerShell：

```powershell
g++ -std=c++23 -O2 -pthread test.cpp -o test.exe -latomic
.\test.exe
```

## 说明

- `DynamicPool` 兼容原来的通用提交方式。
- `DynamicMoveOnlyPool` 用于减少 `std::function` 包装 `std::packaged_task` 时需要的额外共享状态开销。
- `StaticPool<FuncType>` 用于固定任务类型，队列直接存储 `FuncType`，不做类型擦除。
- `LockFreeQueue<T>` 当前需要 `T` 支持默认构造。
