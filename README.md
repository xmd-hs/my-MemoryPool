# my-MemoryPool

基于 C++ 实现的多层内存池，用于降低频繁小对象分配时的系统调用开销，并控制多线程下的锁竞争。

仓库包含三个演进版本：

| 版本 | 思路 | 适用 |
|------|------|------|
| **v1** | 按 8 字节分档的定长哈希桶 + 无锁自由链表 | 教学 / 单线程小对象 |
| **v2** | ThreadCache → CentralCache → PageCache | 对照实验 |
| **v3** | 几何 size class、页映射、整 span 归还 OS、可安装 SDK | **推荐用于实际接入** |

## 目录结构

```text
my-MemoryPool/
├── v1/          # 教学版定长池
├── v2/          # 三层缓存对照实现
├── v3/          # 推荐使用的 SDK 版本
│   ├── include/kama/   # 公共头文件
│   ├── src/            # 实现源码
│   ├── tests/          # 单元测试与性能测试
│   └── examples/       # 最小接入示例
└── README.md
```

## v1

多种定长分配器，可替换小对象的 `new` / `delete`：

- `allocate` / `deallocate`：从对应槽位的内存池取还内存
- 自由链表：带版本号的 CAS（tagged pointer），避免 ABA
- 块内 bump 分配，用互斥锁保护扩块

## v2 / v3 架构

三层缓存：

- **ThreadCache**：线程本地自由链表，热路径无锁；线程退出时把残留块还给中心缓存
- **CentralCache**：按 size class 分片自旋锁；v3 按 span 管理自由块，整段空闲后归还 PageCache
- **PageCache**：按系统页向 OS 申请（Linux/macOS `mmap`，Windows `VirtualAlloc`），支持前后合并；完整映射可归还 OS

### v3 相比 v2 的增强

- **几何 size class**（约 60 档，8B–256KB）：每线程元数据从约 512KB 降到数 KB
- **页映射**：`deallocate(ptr)` 可不带 size，跨线程释放安全
- **sized free 快路径**：`deallocate(ptr, size)` 小对象释放可跳过 span 查找
- **并发页映射查询**：`PageCache::findSpan()` 使用读写锁，降低无 size 释放开销
- **可观测统计**：`stats()` 返回分配/释放计数、中心缓存 refill/flush 次数等
- **调试保护**（可选）：重复释放、内部偏移指针、外部指针误传校验
- **异常安全**：`newElement` 构造失败时自动回收原始内存
- **STL 安全**：`Allocator<T>` 带乘法溢出保护

## v3 SDK 使用

v3 提供可安装的静态库（也可编动态库）。接入方只依赖公共头文件，不必把实现源码编进自己的工程。

### 公共头文件

| 头文件 | 用途 |
|--------|------|
| `<kama/MemoryPool.h>` | C++ 主接口：`allocate` / `deallocate` / `newElement` |
| `<kama/Allocator.h>` | STL 分配器，给 `std::vector` 等容器用 |
| `<kama/kama_memory_pool.h>` | C API，方便其它语言绑定 |

### 编译并安装

```bash
cd v3
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install ..
cmake --build .
cmake --install .
```

常用 CMake 选项：

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DKAMA_MEMORY_POOL_BUILD_SHARED=OFF \
      -DKAMA_MEMORY_POOL_BUILD_TESTS=ON \
      -DKAMA_MEMORY_POOL_BUILD_EXAMPLES=ON \
      -DKAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF \
      -DKAMA_MEMORY_POOL_ENABLE_ASAN=OFF \
      -DKAMA_MEMORY_POOL_ENABLE_UBSAN=OFF \
      -DKAMA_MEMORY_POOL_ENABLE_TSAN=OFF \
      ..
```

| 选项 | 说明 |
|------|------|
| `KAMA_MEMORY_POOL_BUILD_SHARED` | 编译动态库（默认 OFF） |
| `KAMA_MEMORY_POOL_BUILD_TESTS` | 编译单元测试与性能测试 |
| `KAMA_MEMORY_POOL_BUILD_EXAMPLES` | 编译 `kama_example` 示例 |
| `KAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS` | 开启释放校验（开发调试用） |
| `KAMA_MEMORY_POOL_ENABLE_ASAN` | AddressSanitizer |
| `KAMA_MEMORY_POOL_ENABLE_UBSAN` | UndefinedBehaviorSanitizer |
| `KAMA_MEMORY_POOL_ENABLE_TSAN` | ThreadSanitizer（不可与 ASan/UBSan 同开） |

安装目录大致为：

```text
include/kama/MemoryPool.h
include/kama/Allocator.h
include/kama/kama_memory_pool.h
lib/libkama_memory_pool.a          # Windows: kama_memory_pool.lib
lib/cmake/KamaMemoryPool/...
```

### 本仓库自测

```bash
cmake --build . --target test    # 单元测试
cmake --build . --target perf    # 性能对照
ctest --output-on-failure        # 标准 CTest 入口
./kama_example                   # SDK 最小示例
```

无 CMake 时也可以直接链静态库源文件（需 C++17）：

```bash
clang++ -std=c++17 -O2 -pthread \
  -Iinclude -Isrc \
  src/*.cpp your_app.cpp \
  -o your_app
```

### 接入到你的 CMake 工程

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(KamaMemoryPool REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE KamaMemoryPool::memory_pool)
```

自定义安装前缀：

```bash
cmake -DKamaMemoryPool_DIR=/path/to/v3/install/lib/cmake/KamaMemoryPool ..
# 或
cmake -DCMAKE_PREFIX_PATH=/path/to/v3/install ..
```

### C++ 用法

```cpp
#include <kama/MemoryPool.h>

using Kama_memoryPool::MemoryPool;

void demo()
{
    void* p = MemoryPool::allocate(64);
    MemoryPool::deallocate(p);          // 无 size 释放

    void* q = MemoryPool::allocate(128);
    MemoryPool::deallocate(q, 128);     // 带 size 释放（性能更好）

    int* n = MemoryPool::newElement<int>(42);
    MemoryPool::deleteElement(n);

    auto st = MemoryPool::stats();
    // st.allocCount / freeCount / liveAllocs
    // st.smallAllocCount / largeAllocCount
    // st.sizedFreeCount / unsizedFreeCount
    // st.centralRefillCount / centralFlushCount
}
```

超过 8 字节对齐的类型会走 `allocateAligned`：

```cpp
struct alignas(32) Block { char data[32]; };
Block* b = Kama_memoryPool::newElement<Block>();
Kama_memoryPool::deleteElement(b);
```

### STL 容器

```cpp
#include <kama/Allocator.h>
#include <vector>

std::vector<int, Kama_memoryPool::Allocator<int>> nums;
nums.push_back(1);
```

### C API

```c
#include <kama/kama_memory_pool.h>

void* p = kama_alloc(64);
kama_free(p);

void* q = kama_alloc_aligned(64, 16);
kama_free_sized(q, 64);
```

### 使用注意

- 用池分配的指针必须用池释放，不要和 `malloc` / `free`、`new` / `delete` 混用。
- `deallocate(ptr)` 通过页映射识别块大小，跨线程释放是安全的。
- 已知分配大小时，优先使用 `deallocate(ptr, size)`，可走更快路径。
- 小对象上限是 256KB（`kMaxBytes`）；更大的请求按页向 OS 申请，释放时整段归还。
- 默认最小对齐 8 字节（`kAlignment`）。
- 分配失败返回 `nullptr`；`Allocator<T>` 失败时抛 `std::bad_alloc`。
- 当前版本号：`1.0.0`（`KAMA_MEMORY_POOL_VERSION`）。

### 调试保护

开发阶段可开启调试保护，帮助尽早发现错误用法：

```bash
cmake -DKAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS=ON ..
```

开启后会额外校验：

- 重复释放
- span 内部偏移指针（非块起始地址）
- 明显的外部指针误传

调试保护有一定性能开销，**不建议在生产环境默认开启**。

## 编译 v1 / v2（教学对照）

v1、v2 仍是目录内直接编测试程序，不是 SDK：

```bash
cd v1   # 或 v2
mkdir build && cd build
cmake ..
cmake --build .
```

## 测试

### 功能测试

| 版本 | 覆盖 |
|------|------|
| v2 | 基础分配、写入、多线程、边界、压力 |
| v3 | 上述 + size class、无 size 释放、对齐与 `newElement`、线程退出回血、统计、跨线程释放、异常安全、无泄露回归、调试保护 |

### 测试环境

| 项 | 值 |
|----|----|
| CPU | Apple M5（arm64，10 核） |
| 内存 | 16 GB |
| 系统 | macOS 26.5 |
| 编译器 | Apple clang 21.0.0，`-O2` |

Apple 平台的系统分配器（libmalloc）本身很快，下列数字用于对比本仓库各版本，而不是宣称「一定快过所有 malloc」。

### v1 性能

负载：每轮 `10000` 次分配，每次分配/释放 4 种小对象（约 4B–80B），共 10 轮。表中为**各线程耗时之和**（不是墙钟）。

| 线程数 | 内存池（中位数） | `new`/`delete`（中位数） | 相对系统分配器 |
|--------|------------------|--------------------------|----------------|
| 1 | 3.666 ms | 6.662 ms | **约 1.8× 更快** |
| 2 | 77.276 ms | 79.519 ms | 基本持平 |
| 5 | 736.364 ms | 80.846 ms | 明显更慢（扩块互斥锁竞争） |

结论：v1 单线程小对象有优势；线程一多，中心扩块锁会成为瓶颈，这是做 v2/v3 的原因。

### v2 性能

墙钟时间。负载：小对象 50000 次 `{8,16,32,64,128,256}` 轮转；多线程 4×25000；混合 100000 次。

| 场景 | 内存池（中位数） | `new`/`delete`（中位数） |
|------|------------------|--------------------------|
| 小对象 | 2.491 ms | 1.533 ms |
| 4 线程 | 11.660 ms | 4.044 ms |
| 混合尺寸 | 2.898 ms | 2.556 ms |

### v3 性能

Windows / MSVC Release 的最新三路对比（v3、`new/delete`、`malloc/free`）见
[v3/WINDOWS_BENCHMARK.md](v3/WINDOWS_BENCHMARK.md)。生产性能构建默认关闭全局统计，
需要调用 `MemoryPool::stats()` 获取计数时请显式设置
`KAMA_MEMORY_POOL_ENABLE_STATS=ON`。

墙钟时间。`v3/tests/PerformanceTest.cpp` 使用固定 seed、线程局部随机数，每项跑 5 轮取中位数。负载与 v2 不完全相同，不要直接横向对比。

| 场景 | 内存池（中位数） | `new`/`delete`（中位数） |
|------|------------------|--------------------------|
| 小对象 32B × 100000 | 2.688 ms | 2.463 ms |
| 4 线程 × 25000 | 12.491 ms | 4.563 ms |
| 混合尺寸 × 50000 | 6.151 ms | 2.225 ms |

五轮原始数据（单位 ms）：

| 场景 | 池 Run1 | 池 Run2 | 池 Run3 | 池 Run4 | 池 Run5 | new Run1 | new Run2 | new Run3 | new Run4 | new Run5 |
|------|---------|---------|---------|---------|---------|----------|----------|----------|----------|----------|
| 小对象 32B × 100000 | 3.398 | 2.997 | 2.688 | 2.494 | 2.483 | 2.951 | 2.463 | 2.606 | 2.406 | 2.087 |
| 4 线程 × 25000 | 15.071 | 11.531 | 11.101 | 13.671 | 12.491 | 5.171 | 4.594 | 4.563 | 4.234 | 4.291 |
| 混合尺寸 × 50000 | 5.182 | 6.151 | 6.531 | 5.663 | 6.472 | 5.219 | 2.585 | 2.163 | 2.225 | 2.112 |

同一轮测试里 `stats()` 输出为：

```text
allocCount=1255000 freeCount=1255000 liveAllocs=0
smallAllocCount=1255000 largeAllocCount=0
sizedFreeCount=1255000 unsizedFreeCount=0
centralRefillCount=707996 centralFlushCount=656307
```

### 怎么读这些数字

- **v1** 适合说明「定长池在单线程能赢」，也适合说明「一把全局锁在多线程会输得很明显」。
- **v2 / v3** 热路径走线程本地缓存，多线程不再像 v1 那样随线程数崩溃，但在 Apple libmalloc 上仍可能略慢或接近。池的价值更多在：稳定的 size class、可归还 OS、跨平台页分配、可控的调试与统计能力，以及 malloc 较慢的环境（部分 Linux glibc、高碎片服务）。
- 复现：在 `v3/build` 编译后运行 `perf_test`。v1 已对 `new`/`delete` 做防优化处理（函数指针 + compiler barrier），否则 `-O2` 会把空对象的 new/delete 整段消掉。

## 许可证

本项目仅供学习与研究使用。
