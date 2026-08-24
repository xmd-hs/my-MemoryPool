# my-MemoryPool

面向高频小对象分配的 C++17 内存池。推荐使用 `v3`：通过线程本地缓存、按 size class 分片的中央缓存和页缓存，减少通用分配器调用与多线程锁竞争，并提供 C++、STL 和 C ABI 接口。

## 性能数据

以下均为 Release 构建、统计和 Debug Guards 关闭、5 次运行中位数，单位为毫秒。结果依赖硬件、编译器和负载，仅用于参考。

### Windows / MSVC

| 场景 | v3 | new/delete | malloc/free |
|---|---:|---:|---:|
| 32B，100000 次 | 2.564 | 5.395 | 5.401 |
| 4 线程，每线程 25000 次 | 3.069 | 5.596 | 5.937 |
| 16B-2048B 混合，50000 次 | 4.700 | 13.978 | 15.357 |
| 4 线程跨线程释放，64B x 50000 | 2.129 | 2.587 | 2.313 |

扩展矩阵（节选）：

| 大小 / 线程 | v3 | new/delete | malloc/free |
|---|---:|---:|---:|
| 16B / 1 | 0.400 | 1.101 | 1.012 |
| 32B / 8 | 0.777 | 2.512 | 2.578 |
| 256B / 8 | 0.744 | 3.502 | 3.313 |
| 1KB / 8 | 0.813 | 5.188 | 4.396 |
| 4KB / 8 | 1.149 | 5.959 | 4.696 |
| 64KB / 8 | 84.162 | 638.919 | 639.161 |

### Linux / Ubuntu 22.04 / GCC

这是已完成的真实 runner 测量记录，不是当前自动 CI 结果（Linux 自动测试工作流已删除）。

| 场景 | v3 | new/delete | malloc/free |
|---|---:|---:|---:|
| 32B，100000 次 | 2.683 | 4.120 | 4.013 |
| 4 线程，每线程 25000 次 | 3.977 | 4.032 | 2.374 |
| 16B-2048B 混合，50000 次 | 2.832 | 8.398 | 7.876 |
| 4 线程跨线程释放，64B x 50000 | 1.751 | 1.405 | 1.199 |

扩展矩阵（节选）：

| 大小 / 线程 | v3 | new/delete | malloc/free |
|---|---:|---:|---:|
| 16B / 1 | 0.325 | 0.624 | 0.516 |
| 16B / 8 | 1.423 | 3.063 | 2.216 |
| 32B / 8 | 1.665 | 2.979 | 2.107 |
| 256B / 8 | 1.717 | 3.689 | 3.910 |
| 1KB / 8 | 1.507 | 5.392 | 4.477 |
| 4KB / 8 | 1.988 | 4.421 | 3.796 |
| 64KB / 8 | 36.307 | 172.518 | 174.898 |

Linux 测试期间 `reservedBytes=67534848`、`cachedPageBytes=65175552`，页缓存上限为 64 MiB。Linux 的 4 线程和跨线程释放仍可能落后于 glibc。

### macOS / Apple Silicon / Clang

以下为 CentralCache 分片、两阶段回收与跨线程 remote free 优化后的本地复测（Release，统计关闭）。

| 场景 | v3 | new/delete | malloc/free |
|---|---:|---:|---:|
| 32B，100000 次 | 2.634 | 1.896 | 1.580 |
| 4 线程，每线程 25000 次 | 3.509 | 4.535 | 4.534 |
| 16B-2048B 混合，50000 次 | 1.813 | 2.475 | 1.890 |
| 4 线程跨线程释放，64B x 50000 | 1.385 | 0.973 | 0.783 |

扩展矩阵（节选）：

| 大小 / 线程 | v3 | new/delete | malloc/free |
|---|---:|---:|---:|
| 16B / 1 | 0.195 | 0.405 | 0.366 |
| 32B / 8 | 0.654 | 1.274 | 0.713 |
| 256B / 8 | 0.428 | 2.238 | 1.505 |
| 1KB / 8 | 33.953 | 10.204 | 8.846 |
| 4KB / 8 | 14.779 | 35.464 | 34.569 |
| 64KB / 8 | 72.672 | 174.005 | 175.966 |

macOS 测试期间 `reservedBytes=67633152`、`cachedPageBytes=62390272`，页缓存上限为 64 MiB。优化后，4 线程主场景与多数小对象多线程矩阵已领先或接近系统分配器（例如 32B/8 从约 24 ms 降到约 0.65 ms）；单线程 32B 与跨线程释放仍可能略慢于 libmalloc，1KB/8 线程仍明显落后。Windows / Linux 表格暂未在本轮复测，仍为优化前基线。

## 项目架构

```text
my-MemoryPool/
├── v1/                         教学版：固定大小池
├── v2/                         三层缓存早期实现
├── v3/                         推荐 SDK
│   ├── include/kama/           公共 C++、STL、C API 头文件
│   ├── src/                    ThreadCache/CentralCache/PageCache
│   ├── tests/                  单元、基准和压力测试
│   ├── examples/               最小接入示例
│   ├── cmake/                  CMake package 配置
└── README.md
```

### v3 架构示意图

```mermaid
flowchart TB
    A[应用代码 / STL 容器 / C API] --> B[MemoryPool API]
    B --> C{请求大小与对齐}
    C -->|小对象| D[ThreadCache\n每线程本地空闲链表]
    D -->|批量 refill / flush| E[CentralCache\nsize class × thread shard]
    E --> F[Span block lists\n分片自旋锁 + remote free]
    F -->|申请或归还 span| G[PageCache\n页映射与 span 管理]
    G --> H[分割 / 相邻合并\n缓存预算控制]
    H --> I[OS pages\nVirtualAlloc / mmap]
    C -->|大对象或对齐请求| G
    G -->|释放完整映射| I
```

```text
应用 / STL / C API
        │
ThreadCache（每线程无锁空闲链表）
        │ 批量 refill / flush
CentralCache（size class × thread shard、remote free）
        │ span 分配与归还
PageCache（页映射、分割、合并、缓存预算）
        │
OS：Windows VirtualAlloc / Linux mmap
```

v3 还提供预计算 size-class 索引、sized-free 快路径、跨线程无 size 释放、span 合并归还 OS、溢出和非法对齐检查，以及可选 Debug Guards、统计和 Sanitizer 支持。

## 构建与测试

### 全面测试覆盖

`v3/tests/UnitTest.cpp` 当前覆盖：

- 基本分配、写入、释放和重复压力分配；
- 0 字节、size class 边界、最大 small object 和大对象路径；
- 1/7/31/255/1023/4097/64KB 对齐请求及 128/256/512/4096 对齐；
- 每个 size class 的边界分配与释放；
- sized/unsized free、跨线程 sized free、线程退出自动回收；
- `newElement`/`deleteElement` 异常安全、STL Allocator 和 C API；
- 非法指针、内部指针、foreign pointer、double free（Debug Guards）；
- 多线程混合流量、长随机流量和大 span 生命周期；
- 统计计数、PageCache 泄漏、缓存页上限和对齐写入校验。

最近一次完整验证（2026-08-24）：

| 平台 | 编译器 | 单元测试 | 性能 smoke | 结果 |
|---|---|---:|---:|---|
| Windows 11 | MSVC 19.44 / VS2022 | 通过 | 通过 | 通过 |
| WSL2 Linux | GCC 15.2 / Release | 通过 | 通过 | 通过 |

Linux Release 性能矩阵覆盖 16/32/64/256/1024/4096/65536B 与 1/2/4/8 线程，并对比 `new/delete`、`malloc/free`。本轮结果中 16B-4096B 的大多数并发组合 pool/malloc 为 `0.24x-0.66x`，跨线程 64B x 50000 为 `0.766x`；64KB 大对象在 1 线程为 `0.146x`，8 线程受系统调度和页映射成本影响约 `1.144x`。性能数据会随 CPU、glibc、后台负载和运行次数变化，应以目标机器重复测试为准。

```bash
cmake -S v3 -B v3/build-release -DCMAKE_BUILD_TYPE=Release \
  -DKAMA_MEMORY_POOL_BUILD_TESTS=ON -DKAMA_MEMORY_POOL_BUILD_EXAMPLES=ON \
  -DKAMA_MEMORY_POOL_ENABLE_STATS=OFF -DKAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF
cmake --build v3/build-release --parallel
cmake --build v3/build-release --target run_unit_tests
cmake --build v3/build-release --target perf
ctest --test-dir v3/build-release --output-on-failure
```

性能测试覆盖 16/32/64/256/1024/4096/65536B、1/2/4/8 线程、混合尺寸、实际内存写入和跨线程所有权转移，并对比 `new/delete` 与 `malloc/free`。Linux 可手动使用 `v3/Dockerfile.linux_perf` 复现实验环境；该 Docker 文件不是自动 CI。所有已测数据均集中维护在本 README，不再依赖额外的原始记录文件。

## 使用示例

```cpp
#include <kama/MemoryPool.h>
using Kama_memoryPool::MemoryPool;

void demo() {
    void* p = MemoryPool::allocate(64);
    MemoryPool::deallocate(p);
    void* q = MemoryPool::allocate(128);
    MemoryPool::deallocate(q, 128); // 已知大小时更快
    int* value = MemoryPool::newElement<int>(42);
    MemoryPool::deleteElement(value);
}
```

STL：`std::vector<int, Kama_memoryPool::Allocator<int>> values;`

C API：`kama_alloc(64)`、`kama_free(ptr)`、`kama_alloc_aligned(size, alignment)`、`kama_free_sized(ptr, size)`。

## 注意事项

- 池分配的指针必须使用本项目接口释放，不得与 `free`、`delete` 混用。
- 超过小对象上限的请求按页向 OS 申请；失败返回 `nullptr`，STL 分配器抛出 `std::bad_alloc`。
- 生产环境建议关闭统计、Debug Guards 和 Sanitizer；开发和 CI 中开启以发现误用。
- 发布前应在目标平台使用 ASan、UBSan、TSan 和长时间随机压力测试。

## 许可证

本项目当前用于学习与研究，正式商用前请结合自身合规要求进行审查。
