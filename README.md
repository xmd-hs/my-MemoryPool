## 项目介绍

本项目是基于 C++ 实现的多层内存池，用来降低频繁小对象分配时的系统调用开销，并控制多线程下的锁竞争。仓库里有三个实现：

| 版本 | 思路 | 适用 |
|------|------|------|
| **v1** | 按 8 字节分档的定长哈希桶 + 无锁自由链表 | 教学 / 单线程小对象 |
| **v2** | ThreadCache → CentralCache → PageCache，延迟把空闲 span 还给页缓存 | 对照实验 |
| **v3** | 在 v2 三层结构上改为几何 size class、页映射、整 span 归还 OS | **推荐用于实际接入** |

### v1

多种定长分配器，可替换小对象的 `new` / `delete`：

- `allocate` / `deallocate`：从对应槽位的内存池取还内存
- 自由链表：带版本号的 CAS（tagged pointer），避免 ABA
- 块内bump分配，用互斥锁保护扩块

架构：

![v1 架构](images/v1.jpg)

### v2 / v3

三层缓存：

- **ThreadCache**：线程本地自由链表，热路径无锁；线程退出时把残留块还给中心缓存
- **CentralCache**：按 size class 分片自旋锁；v3 按 span 管理自由块，整段空闲后归还 PageCache
- **PageCache**：按系统页向 OS 申请（Linux/macOS `mmap`，Windows `VirtualAlloc`），支持前后合并；完整映射可归还 OS
- **几何 size class**（约 60 档，8B–256KB）：每线程元数据从约 512KB 降到数 KB
- **页映射**：`deallocate(ptr)` 可不带 size；支持对齐分配、`newElement` / `deleteElement` 和简单统计

架构：

![v2/v3 架构](images/v2.png)

v3 对外接口：

```cpp
void* MemoryPool::allocate(size_t size);
void* MemoryPool::allocateAligned(size_t size, size_t alignment);
void  MemoryPool::deallocate(void* ptr);              // 不需要带 size
void  MemoryPool::deallocate(void* ptr, size_t size); // 兼容旧调用
T*    newElement<T>(args...);
void  deleteElement<T>(T*);
MemoryPoolStats MemoryPool::stats();
```

## 编译

Linux / macOS / Windows 均可。先进入 `v1`、`v2` 或 `v3`：

```bash
cd v3
mkdir build && cd build
cmake ..
cmake --build .
```

Windows 示例：

```bash
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

无 CMake 时也可直接用编译器（需 C++17；v1 为 C++11）：

```bash
clang++ -std=c++17 -O2 -pthread -Iinclude src/*.cpp tests/UnitTest.cpp -o unit_test
```

清理：

```bash
cmake --build . --target clean
```

## 运行

Linux / macOS：

```bash
./unit_test    # v1 可执行文件名以 CMake 工程名为准
./perf_test
```

Windows：

```text
unit_test.exe
perf_test.exe
```

v2 / v3 也可用：

```bash
cmake --build . --target test
cmake --build . --target perf
```

## 测试结果

测试环境（2026-08-18）：

| 项 | 值 |
|----|----|
| CPU | Apple M5（arm64，10 核） |
| 内存 | 16 GB |
| 系统 | macOS 26.5 |
| 编译器 | Apple clang 21.0.0，`-O2` |
| 方法 | 每项跑 3 次，取**中位数**；对照为同一进程内的 `new`/`delete`（libmalloc） |

Apple 平台的系统分配器本身很快，下列数字用来对比本仓库三个版本，而不是宣称「一定快过全世界的 malloc」。

### 功能测试

| 版本 | 结果 |
|------|------|
| v2 | basic / writing / multi-thread / edge / stress **全部通过** |
| v3 | 上述用例 + size class / 无 size 释放 / 对齐与 `newElement` / 线程退出回血 **全部通过** |

### v1 性能

负载：每轮 `10000` 次分配，每次分配/释放 4 种小对象（约 4B–80B），共 10 轮。表中为**各线程耗时之和**（不是墙钟）。

| 线程数 | 内存池（中位数） | `new`/`delete`（中位数） | 相对系统分配器 |
|--------|------------------|--------------------------|----------------|
| 1 | 3.666 ms | 6.662 ms | **约 1.8× 更快** |
| 2 | 77.276 ms | 79.519 ms | 基本持平 |
| 5 | 736.364 ms | 80.846 ms | 明显更慢（扩块互斥锁竞争） |

三次原始数据：

| 线程 | 池 Run1 / 2 / 3 | new Run1 / 2 / 3 |
|------|-----------------|------------------|
| 1 | 5.307 / 3.620 / 3.666 | 6.348 / 6.804 / 6.662 |
| 2 | 77.276 / 73.872 / 80.052 | 73.493 / 79.519 / 79.810 |
| 5 | 716.169 / 736.364 / 762.731 | 80.846 / 86.825 / 63.972 |

结论：v1 单线程小对象有优势；线程一多，中心扩块锁会成为瓶颈，这是做 v2/v3 的原因。

### v2 性能

墙钟时间。负载：

- 小对象：50000 次，大小在 `{8,16,32,64,128,256}` 间轮转，约 1/4 立即释放
- 多线程：4 × 25000 次，同上尺寸，夹杂批量释放
- 混合：100000 次，固定若干档尺寸

| 场景 | 内存池（中位数） | `new`/`delete`（中位数） |
|------|------------------|--------------------------|
| 小对象 | 2.491 ms | 1.533 ms |
| 4 线程 | 11.660 ms | 4.044 ms |
| 混合尺寸 | 2.898 ms | 2.556 ms |

三次原始数据（池 / new，单位 ms）：

| 场景 | Run1 | Run2 | Run3 |
|------|------|------|------|
| 小对象 | 3.604 / 2.821 | 2.491 / 1.533 | 2.052 / 1.365 |
| 4 线程 | 17.989 / 4.140 | 11.660 / 4.044 | 11.158 / 2.989 |
| 混合 | 3.568 / 3.134 | 2.898 / 2.556 | 2.496 / 2.475 |

### v3 性能

墙钟时间。负载与 v2 **不完全相同**（小对象是 100000 次 32B；混合是 50000 次 `{16…2048}`），不要和 v2 同一行硬比。多线程同为 4 × 25000，可对照。

| 场景 | 内存池（中位数） | `new`/`delete`（中位数） |
|------|------------------|--------------------------|
| 小对象 32B × 100000 | 2.020 ms | 1.945 ms |
| 4 线程 × 25000 | 11.206 ms | 6.221 ms |
| 混合尺寸 × 50000 | 3.897 ms | 3.228 ms |

三次原始数据（池 / new，单位 ms）：

| 场景 | Run1 | Run2 | Run3 |
|------|------|------|------|
| 小对象 | 2.617 / 2.335 | 2.020 / 1.914 | 1.739 / 1.945 |
| 4 线程 | 13.119 / 7.015 | 11.206 / 5.992 | 10.682 / 6.221 |
| 混合 | 3.673 / 3.228 | 3.897 / 2.997 | 4.750 / 4.372 |

同负载对照：4 线程场景 v3 中位数 11.206 ms，v2 为 11.660 ms，v3 略好。小对象第三轮 v3 池（1.739 ms）快于 `new`/`delete`（1.945 ms），但中位数仍与系统分配器接近。

### 怎么读这些数字

- **v1** 适合说明「定长池在单线程能赢」，也适合说明「一把全局锁在多线程会输得很明显」。
- **v2 / v3** 热路径走线程本地缓存，多线程不再像 v1 那样随线程数崩溃，但在 Apple libmalloc 上仍可能略慢。池的价值更多在：稳定的 size class、可归还 OS、跨平台页分配、以及 malloc 较慢的环境（部分 Linux glibc、高碎片服务）。
- 复现：在对应目录编译后运行 `unit_test` / `perf_test`。v1 已对 `new`/`delete` 做防优化处理（函数指针 + compiler barrier），否则 `-O2` 会把空对象的 new/delete 整段消掉。
