<!-- markdownlint-disable MD013 MD033 MD060 -->

# my-MemoryPool

C++14 内存池。日常用 **v3**：线程本地缓存 → 按 size class 分片的中央缓存（含 lock-free Transfer）→ 页缓存，用来摊掉系统分配器调用和多线程锁竞争。

小对象覆盖 **8B–512KiB**。池里拿到的指针必须用本项目接口释放，不能和 `free` / `delete` 混用。非法输入返回 `nullptr`；`Allocator<T>` 把失败转成 `std::bad_alloc`。

## 对外 API

头文件在 `v3/include/kama/`。

### C++：`MemoryPool.h`

```cpp
namespace Kama_memoryPool {

constexpr std::size_t kAlignment = 8;
constexpr std::size_t kMaxBytes  = 512 * 1024;   // 512KiB

class MemoryPool {
public:
    static void* allocate(std::size_t size);
    static void* allocateAligned(std::size_t size, std::size_t alignment);
    static void  deallocate(void* ptr);                  // unsized
    static void  deallocate(void* ptr, std::size_t size); // sized，更快
    static void  trim();                                 // 只收当前线程多余缓存
    static void  setPageCacheLimit(std::size_t bytes);   // 全局空闲页预算
    static std::size_t pageCacheLimit();
    static void  setLargeCacheLimit(std::size_t bytes);  // 每线程大对象预算
    static std::size_t largeCacheLimit();
    static void  releaseUnused();                        // 显式回收可用缓存
    static MemoryPoolStats stats();

    template<typename T, typename... Args>
    static T* newElement(Args&&... args);   // 构造失败会释放内存并重新抛出

    template<typename T>
    static void deleteElement(T* p);
};

template<typename T, typename... Args>
T* newElement(Args&&... args);

template<typename T>
void deleteElement(T* p);
}
```

| 函数 | 行为 |
|---|---|
| `allocate(size)` | `size == 0` 按 8 字节处理；`size >= kMaxBytes` 走大对象 span |
| `allocateAligned(size, alignment)` | `alignment` 必须是 2 的幂且非 0，否则 `nullptr`；`alignment <= 8` 时等同 `allocate` |
| `deallocate(ptr)` | `ptr == nullptr` 为空操作；页尾 PageTag（≤256B）或基数页表恢复 size class |
| `deallocate(ptr, size)` | 小对象按 size 归档；大对象仍需查找 Span 元数据 |
| `trim()` | 轻量整理当前线程超水位的小对象；保留大对象热缓存 |
| `releaseUnused()` | 交出当前线程缓存、排空 Transfer，并回收可释放的完整空闲页段 |
| `stats()` | `reservedBytes` / `cachedPageBytes` 始终有效；其余计数需打开 `KAMA_MEMORY_POOL_ENABLE_STATS` |

### C++：`Allocator.h`

STL 兼容分配器，可用于 `std::vector<T, Kama_memoryPool::Allocator<T>>`。

```cpp
template<typename T>
class Allocator {
public:
    using value_type = T;
    T* allocate(std::size_t n);                 // 失败抛 std::bad_alloc
    void deallocate(T* p, std::size_t n) noexcept;
};
```

`alignof(T) <= 8` 时走 sized 路径；更大对齐走 `allocateAligned` + unsized free。无状态：`is_always_equal`，任意两个 `Allocator` 相等。

### C：`kama_memory_pool.h`

```c
void* kama_alloc(size_t size);
void* kama_alloc_aligned(size_t size, size_t alignment);
void  kama_free(void* ptr);
void  kama_free_sized(void* ptr, size_t size);
void  kama_trim(void);
void  kama_set_page_cache_limit(size_t bytes);
size_t kama_page_cache_limit(void);
void  kama_set_large_cache_limit(size_t bytes);
size_t kama_large_cache_limit(void);
void  kama_release_unused(void);
```

分别对应 `allocate` / `allocateAligned` / `deallocate` / `deallocate(ptr, size)` / `trim`。

### 示例

```cpp
#include <kama/MemoryPool.h>
#include <kama/Allocator.h>
#include <kama/kama_memory_pool.h>
#include <vector>

void demo() {
    using namespace Kama_memoryPool;

    void* p = MemoryPool::allocate(64);
    MemoryPool::deallocate(p, 64);

    int* n = MemoryPool::newElement<int>(7);
    MemoryPool::deleteElement(n);

    std::vector<int, Allocator<int>> values;
    values.push_back(42);

    void* q = kama_alloc(32);
    kama_free(q);

    MemoryPool::trim();   // 空闲点收回本线程多余缓存
}
```

## 仓库结构

```text
my-MemoryPool/
|-- v1/     教学版：固定槽 + 带 tag 的无锁自由链表
|-- v2/     三层缓存早期实现
|-- v3/     推荐版本
|   |-- include/kama/     对外头文件
|   |-- src/              ThreadCache / CentralCache / PageCache
|   |-- tests/            unit_test、perf_test
|   |-- examples/
|   `-- CMakeLists.txt
`-- README.md
```

日常只用 v3。v1 / v2 留作对照。

| 版本 | 模型 | 小对象上限 |
|---|---|---|
| v1 | 每档一个固定槽池 | 512B |
| v2 | 三层缓存初版 | 256KiB |
| v3 | 三层 + 8 分片 + Transfer + 页标签 | 512KiB |

## 架构

```mermaid
flowchart TB
    A[应用 / STL / C API] --> B[MemoryPool]
    B --> C{大小与对齐}
    C -->|小于 512KiB| D[ThreadCache<br/>线程本地空闲链表]
    D -->|批量补给 / 溢出 / 线程退出| E[CentralCache]
    E --> E1[size class x 16 分片]
    E --> E2[lock-free Transfer]
    E1 --> F[Span]
    F --> G[PageCache]
    C -->|大对象或特殊对齐| H[TLS 大 span 缓存 默认 8MiB/线程]
    H -->|miss| G
    G --> I[VirtualAlloc / mmap]
    G --> J[三级 lock-free 基数页表]
```

同线程小对象只在 ThreadCache 链表上 pop/push。跨线程释放进入执行释放线程的 ThreadCache，超水位或线程退出后通过 Transfer 供其他线程复用。中央层处理批量补给、Transfer 和分片 steal。页层向系统申请/归还页，默认空闲页预算 32MiB。

```mermaid
flowchart TD
    A[allocate] --> B{大于等于 512KiB?}
    B -->|是| L[TLS 大 span 缓存 → PageCache]
    B -->|否| C[size class]
    C --> D{TLS 有空闲块?}
    D -->|是| E[弹出并 prefetch 下一块]
    D -->|否| F[CentralCache.fetchRange]
    F --> G[本分片 → Transfer → steal → 新 span]
```

```mermaid
flowchart TD
    A[deallocate] --> B{带 size 且小于 512KiB?}
    B -->|是| C[SizeClass::getIndex]
    B -->|否| D[PageTag 或基数页表 / 大对象 findLargeSpan]
    C --> E[插入本线程 TLS]
    D --> E
    E -->|超水位| F[pushTransfer]
    F -->|Transfer 满| G[returnRange 回所属 span]
    H[线程退出] --> I[TLS 整链 pushTransfer]
```

unsized free（只给指针）是相对 glibc 的关键路径：`blockSize <= 256` 的页在页尾写 `PageTag`，同页 O(1) 恢复 size class；失效再走无锁基数页表。

### 缓存预算与回收

MemoryPool::setPageCacheLimit 设置进程级空闲页预算，默认 32MiB；MemoryPool::setLargeCacheLimit 设置每线程大对象缓存预算，默认 8MiB。两个预算都接受 0，表示不保留对应缓存。降低页预算会立即释放完整空闲的系统申请段；部分空闲段仍有占用，不能归还，因此页预算是可回收部分的软上限。降低大对象预算立即约束调用线程，其他线程在下次使用该缓存时收缩。

MemoryPool::releaseUnused 交出调用线程的全部空闲块和大对象缓存、排空当时可见的 Transfer 块，再归还 PageCache 中完整空闲的系统申请段。其他线程的 TLS 缓存与活动分配不会被强制回收；并发调用按尽力而为处理。原有 trim 仍只收缩调用线程的超水位缓存。C API 提供 kama_set_page_cache_limit、kama_set_large_cache_limit、kama_release_unused 及两个预算查询函数。

## 构建

CMake 3.14+、C++14。未开 sanitizer 时 Release 会尝试打开 LTO/LTCG。

```bash
cmake -S v3 -B v3/build-release -DCMAKE_BUILD_TYPE=Release \
  -DKAMA_MEMORY_POOL_BUILD_TESTS=ON -DKAMA_MEMORY_POOL_BUILD_EXAMPLES=ON
cmake --build v3/build-release --config Release --parallel
```

### 二进制交付

建议用共享库制作独立安装包。以下命令生成 Windows 的 ZIP 或 Linux 的 TGZ，归档只包含公开头文件、库文件及 CMake 包配置，不包含 `v3/src`、测试或示例源码：

```bash
cmake -S v3 -B build-binary -DCMAKE_BUILD_TYPE=Release -DKAMA_MEMORY_POOL_BUILD_SHARED=ON -DKAMA_MEMORY_POOL_BUILD_TESTS=OFF -DKAMA_MEMORY_POOL_BUILD_EXAMPLES=OFF
cmake --build build-binary --config Release --parallel
cpack --config build-binary/CPackConfig.cmake -C Release -B dist
```

只将 `dist` 中对应系统、架构和编译器的归档交给其他项目。解压后可直接使用已导出的 CMake 目标：

```cmake
find_package(KamaMemoryPool 1 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE KamaMemoryPool::memory_pool)
```

配置使用方时将解压目录传给 `CMAKE_PREFIX_PATH`。Windows 共享库的 DLL 须在程序目录或 `PATH` 中；Linux 的 `.so` 须在运行时搜索路径中。若不希望部署共享库，可将构建选项改为 `KAMA_MEMORY_POOL_BUILD_SHARED=OFF` 来生成静态库包。预编译包须与目标系统、架构和编译器 ABI 匹配；跨语言接入可使用公开的 C 接口。分配和释放必须通过同一个库实例完成。公开头文件是编译 C/C++ 调用方所必需的；`Allocator<T>` 等模板包装仍在头文件中，核心内存池实现只存在于二进制中。二进制包不能防止逆向分析。

| 选项 | 默认 | 说明 |
|---|---|---|
| `KAMA_MEMORY_POOL_BUILD_TESTS` | ON | `unit_test` / `perf_test` |
| `KAMA_MEMORY_POOL_BUILD_EXAMPLES` | ON | `kama_example` |
| `KAMA_MEMORY_POOL_BUILD_SHARED` | OFF | 共享库 |
| `KAMA_MEMORY_POOL_ENABLE_STATS` | OFF | 热路径原子计数，测速请关 |
| `KAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS` | OFF | double free / 错误 size |
| `KAMA_MEMORY_POOL_ENABLE_ASAN` / `UBSAN` / `TSAN` | OFF | TSan 不能和 ASan/UBSan 一起 |

```bash
cmake --build v3/build-release --config Release --target run_unit_tests
cmake --build v3/build-release --config Release --target perf
```

Linux：`bash v3/run_linux_quick.sh [build_dir]`，或 `docker build -f v3/Dockerfile.linux_perf -t kama-mp v3`。WSL 下可将 `build_dir` 指向 Linux 文件系统以避免跨文件系统构建开销。

## 测试

`unit_test` 覆盖：小/中/大对象、size class 边界、对齐、非法输入、C API、STL Allocator、`newElement` 异常回滚、`trim`、多线程、跨线程 sized/unsized free、线程退出、页缓存上限、页复用后再 unsized、长随机压力。

`perf_test` 对比 `MemoryPool` / `new[]+delete[]` / `malloc+free`。目前有 **51 项计分比较与 1 项 trim 维护成本信息项**。核心场景 9 次取中位数，可扩展矩阵 3 次取中位数。快于基线 5% 为 WIN，慢于 5% 为 LOSE，否则 TIE。默认任一 LOSE 返回非 0；`perf_test --report` 只报告结果，CTest 使用此模式，避免将性能噪声当成正确性失败。

`workload_bench [pool|malloc] [threads] [ops_per_thread]` 在独立进程运行混合尺寸、不同生命周期和有界跨线程释放队列，报告吞吐、采样 P50/P99、请求存活峰值、Linux RSS 及显式回收前后的预留量。比较两个后端时应使用相同参数，多次运行；RSS 与请求存活峰值不能直接相减作为碎片率。

## 性能数据

以下表格为 **2026-09-07 历史套件输出**（提交 `2855c59`，未四舍五入），单位毫秒，越小越好。统计和 Debug Guards 关闭。换机器或负载后请自行跑当前版本的 `perf_test`。

后续维护（`trim`、页标签收小、大对象 8MiB 字节上限等）没有改分配语义。Windows 上后来复测核心套件仍是 **48 WIN / 0 TIE / 0 LOSE**。

### Windows 11 / MSVC Release + LTCG

`unit_test` 通过。`perf_test`：**48 WIN / 0 TIE / 0 LOSE**。结束后 `reservedBytes=108134400`（103.12 MiB），`cachedPageBytes=65929216`（62.88 MiB）。

| 场景 | v3 | new/delete | malloc/free | vs new | vs malloc |
|---|---:|---:|---:|---|---|
| 32B sized ×100000 | 1.713 | 4.665 | 4.471 | WIN | WIN |
| 32B unsized ×100000 | 1.517 | 4.440 | 4.762 | WIN | WIN |
| 4 线程 sized ×100000 | 6.445 | 22.325 | 20.284 | WIN | WIN |
| 4 线程 unsized ×100000 | 7.454 | 18.699 | 17.821 | WIN | WIN |
| 混合 16B–2048B sized ×50000 | 2.311 | 6.863 | 4.332 | WIN | WIN |
| 混合 16B–2048B unsized ×50000 | 2.808 | 3.787 | 3.410 | WIN | WIN |
| 跨线程 sized 64B ×50000 | 1.438 | 1.966 | 1.803 | WIN | WIN |
| 跨线程 unsized 64B ×50000 | 1.275 | 1.968 | 1.801 | WIN | WIN |
| 流水线 sized 64B ×50000 | 3.260 | 8.703 | 8.359 | WIN | WIN |
| 流水线 unsized 64B ×50000 | 3.341 | 9.254 | 8.634 | WIN | WIN |
| 高存活 64B ×100000 | 1.811 | 3.623 | 4.318 | WIN | WIN |
| 对齐 96B / 64-align ×50000 | 1.248 | 1.881 | 1.781 | WIN | WIN |
| 大对象 512KB ×200 | 0.005 | 0.283 | 0.289 | WIN | WIN |
| STL Allocator ×4000 | 0.279 | 0.317 | 0.330 | WIN | WIN |
| 冷 512KB ×100（1 存活） | 0.002 | 0.111 | 0.112 | WIN | WIN |
| vector 2e4 ints ×200 | 2.810 | 4.841 | 4.799 | WIN | WIN |
| size-class churn sized | 0.019 | 0.063 | 0.056 | WIN | WIN |
| size-class churn unsized | 0.022 | 0.054 | 0.053 | WIN | WIN |
| 8 线程 unsized burst ×50000 | 5.705 | 12.598 | 10.195 | WIN | WIN |
| 高存活 unsized 64B | 2.245 | 4.027 | 3.830 | WIN | WIN |

可扩展矩阵（每线程 25000 次，3 次中位数），全部 WIN。表中为 pool / malloc：

| 大小 | 1 线程 | 2 线程 | 4 线程 | 8 线程 |
|---:|---:|---:|---:|---:|
| 16B | 0.389 / 1.005 | 0.504 / 1.298 | 0.714 / 1.688 | 1.401 / 3.379 |
| 32B | 0.402 / 1.231 | 0.419 / 1.720 | 0.633 / 1.939 | 0.820 / 3.389 |
| 64B | 0.379 / 1.173 | 0.425 / 1.544 | 0.502 / 1.884 | 0.756 / 3.140 |
| 256B | 0.425 / 1.205 | 0.460 / 1.614 | 0.637 / 1.957 | 0.990 / 3.424 |
| 1KB | 0.459 / 1.083 | 0.428 / 1.471 | 0.531 / 2.009 | 0.900 / 3.919 |
| 4KB | 0.512 / 1.795 | 0.512 / 1.673 | 0.591 / 2.289 | 1.073 / 4.468 |
| 64KB | 0.537 / 34.988 | 0.672 / 109.526 | 1.013 / 331.119 | 1.611 / 970.413 |

### Linux / Ubuntu 22.04 / GCC Release + LTO（历史基准，WSL，`-O3 -march=native`）

`unit_test` 通过。`perf_test`：**46 WIN / 2 TIE / 0 LOSE**。结束后 `reservedBytes=99876864`（95.25 MiB），`cachedPageBytes=65798144`（62.75 MiB）。

上面的数据对应旧版测试与环境，不代表当前代码。2026-11-08 在 WSL2 / GCC 15 / C++14 Release + LTO 的当前代码上，两次 `perf_test --report` 分别得到 **45/4/2** 与 **46/3/2**（WIN/TIE/LOSE，51 项计分）；维护成本另报。微秒级场景和 5% 阈值易受噪声影响，不能据此宣称全场景领先。

同环境下 `workload_bench` 8 线程、每线程 2000 万操作各运行三次：调整前 pool / malloc 吞吐中位数为 **23.84 / 22.43 Mops/s**，pool 峰值 RSS 中位数 **27.14 MiB**，malloc **11.88 MiB**。将页缓存默认预算从 64MiB 降至 32MiB、大于 4KB 且不超过 64KB 的每档线程缓存水位从 256 块降至 64 块后，pool 三次运行的吞吐中位数为 **26.73 Mops/s**，峰值 RSS 中位数为 **22.19 MiB**。调度和队列回退次数的波动较大，吞吐变化不能视为稳定提速；峰值 RSS 仍高于 malloc。pool 每次显式 `releaseUnused()` 后的预留量均为 **0 MiB**。

下表保留旧版的场景示例，不作为当前代码的性能结论。

| 场景 | v3 | new/delete | malloc/free | vs new | vs malloc |
|---|---:|---:|---:|---|---|
| 32B sized ×100000 | 0.888 | 3.296 | 3.173 | WIN | WIN |
| 32B unsized ×100000 | 1.043 | 3.130 | 3.036 | WIN | WIN |
| 4 线程 sized ×100000 | 4.416 | 6.878 | 5.893 | WIN | WIN |
| 4 线程 unsized ×100000 | 5.149 | 7.019 | 6.354 | WIN | WIN |
| 混合 16B–2048B sized ×50000 | 2.016 | 4.402 | 3.236 | WIN | WIN |
| 混合 16B–2048B unsized ×50000 | 1.840 | 3.452 | 3.212 | WIN | WIN |
| 跨线程 sized 64B ×50000 | 0.875 | 2.820 | 2.800 | WIN | WIN |
| 跨线程 unsized 64B ×50000 | 1.085 | 3.075 | 2.848 | WIN | WIN |
| 流水线 sized 64B ×50000 | 5.647 | 14.515 | 14.142 | WIN | WIN |
| 流水线 unsized 64B ×50000 | 5.407 | 14.834 | 14.122 | WIN | WIN |
| 高存活 64B ×100000 | 1.967 | 4.413 | 3.277 | WIN | WIN |
| 对齐 96B / 64-align ×50000 | 0.974 | 1.654 | 1.832 | WIN | WIN |
| 大对象 512KB ×200 | 0.003 | 0.008 | 0.008 | WIN | WIN |
| STL Allocator ×4000 | 0.093 | 0.117 | 0.113 | WIN | WIN |
| 冷 512KB ×100（1 存活） | 0.001 | 0.003 | 0.000 | WIN | TIE |
| vector 2e4 ints ×200 | 1.441 | 1.479 | 1.565 | TIE | WIN |
| size-class churn sized | 0.009 | 0.024 | 0.015 | WIN | WIN |
| size-class churn unsized | 0.010 | 0.024 | 0.015 | WIN | WIN |
| 8 线程 unsized burst ×50000 | 3.647 | 4.446 | 3.873 | WIN | WIN |
| 高存活 unsized 64B | 2.008 | 4.043 | 3.408 | WIN | WIN |

可扩展矩阵全部 WIN（pool / malloc）：

| 大小 | 1 线程 | 2 线程 | 4 线程 | 8 线程 |
|---:|---:|---:|---:|---:|
| 16B | 0.285 / 0.806 | 0.512 / 0.798 | 0.441 / 0.877 | 0.625 / 1.468 |
| 32B | 0.170 / 0.843 | 0.255 / 0.799 | 0.345 / 0.890 | 0.454 / 1.597 |
| 64B | 0.209 / 0.886 | 0.247 / 0.917 | 0.316 / 0.926 | 0.509 / 1.947 |
| 256B | 0.249 / 0.988 | 0.296 / 0.956 | 0.343 / 1.006 | 0.620 / 1.777 |
| 1KB | 0.255 / 0.964 | 0.290 / 0.957 | 0.354 / 1.293 | 0.604 / 2.156 |
| 4KB | 0.531 / 1.287 | 0.530 / 1.254 | 0.551 / 1.285 | 1.040 / 2.068 |
| 64KB | 0.539 / 20.025 | 0.683 / 22.556 | 1.101 / 26.757 | 1.919 / 41.168 |

## 注意事项

- 测速请关闭统计和 Debug Guards。发布前建议在目标平台跑 ASan、UBSan 或 TSan。
- 页缓存默认预算 32MiB，完整空闲 origin 可立即回收；大对象每线程默认预算 8MiB。预算可通过 API 调整。
- `trim()` 只作用于当前线程，不是全局回收。
- 默认关 Guards 时，错误 size / double free 仍可能破坏内部链表。
- 仓库目前没有单独的许可证文件。
