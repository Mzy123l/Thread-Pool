# Thread-Pool

基于 C++20 实现的无锁队列和线程池示例项目。项目核心代码位于 `pool/include`，根目录的 `test.cpp` 提供了基础功能测试和使用示例。

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

## 功能说明

### LockFreeQueue

`lock_free_container::LockFreeQueue<T>` 是一个基于链表的无锁队列：

- 使用原子操作完成入队和出队。
- 通过带版本标记的头尾指针降低 ABA 问题风险。
- 支持多线程并发 `enqueue` 和 `dequeue`。
- 支持自定义分配器。

常用接口：

```cpp
lock_free_container::LockFreeQueue<int> queue;

queue.enqueue(1);
queue.enqueue(2);

int value = 0;
if (queue.dequeue(value)) {
    // value == 1
}

bool isEmpty = queue.empty();
queue.clear();
```

### LockFreeThreadPool

`thread_pool::LockFreeThreadPool<>` 使用无锁队列保存任务，工作线程从队列中取出任务并执行：

- `submit` 提交任意可调用对象，并返回 `std::future`。
- `wait_all` 等待当前已提交任务执行完成。
- `shutdown` 优雅关闭线程池。
- `shutdown_now` 尝试立即关闭并清空未执行任务。
- `active_count`、`total_count`、`completed_count`、`thread_count` 查询线程池状态。

使用示例：

```cpp
#include "pool/include/lock_free_thread_pool.hpp"
#include <iostream>

int main() {
    thread_pool::LockFreeThreadPool<> pool(4);

    auto result = pool.submit([](int a, int b) {
        return a + b;
    }, 10, 20);

    std::cout << result.get() << '\n';
    pool.wait_all();
    pool.shutdown();
}
```

## 编译和运行测试

项目使用 C++20 标准。可以在仓库根目录执行：

```bash
g++ -std=c++20 -O2 -pthread test.cpp -o test
./test
```

如果使用 MinGW 且链接 128 位原子操作时报错，可以追加 `-latomic`：

```bash
g++ -std=c++20 -O2 -pthread test.cpp -o test -latomic
./test
```

Windows PowerShell 下运行：

```powershell
g++ -std=c++20 -O2 -pthread test.cpp -o test.exe -latomic
.\test.exe
```

## 测试内容

`test.cpp` 覆盖以下内容：

- 单线程队列 FIFO 顺序。
- 空队列出队行为。
- 多生产者、多消费者并发入队出队。
- 线程池任务返回值。
- 线程池批量任务执行。
- 线程池异常任务不会导致工作线程退出。
- 线程池任务统计计数。

## 注意事项

- `LockFreeQueue<T>` 当前实现会创建一个哨兵节点，因此 `T` 需要支持默认构造。
- 线程池析构时会调用 `shutdown`，也可以手动调用 `shutdown` 提前关闭。
- 该项目主要用于学习无锁队列和线程池的基本实现思路，实际生产环境还需要更完整的压力测试和平台兼容性验证。
