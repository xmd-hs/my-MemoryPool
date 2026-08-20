# v3 Windows 性能测试

完整 Windows / macOS 对比数据已合并到根目录 [README.md](../README.md) 的「测试」章节。
本文档保留 Windows 侧的详细说明与复现步骤。

## 测试环境

| 项目 | 配置 |
|---|---|
| 系统 | Windows 10，x64 |
| 编译器 | MSVC 19.44.35209 |
| CMake | 3.31.6-msvc6 |
| 构建 | Release |
| 统计 | `KAMA_MEMORY_POOL_ENABLE_STATS=OFF` |
| 调试保护 | `KAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF` |

测试程序为 `tests/PerformanceTest.cpp`。每个场景运行 5 次，表格使用中位数；
内存池、`new[]/delete[]` 和 `malloc/free` 使用相同的尺寸序列与释放模式。

## 中位数结果

| 场景 | v3 内存池 | `new[]/delete[]` | `malloc/free` | v3 相对最快系统方案 |
|---|---:|---:|---:|---:|
| 32B 小对象，100000 次 | 2.322 ms | 5.082 ms | 5.178 ms | 快 54.3% |
| 4 线程，每线程 25000 次 | 2.120 ms | 4.756 ms | 4.722 ms | 快 55.1% |
| 16B-2048B 混合尺寸，50000 次 | 2.812 ms | 10.017 ms | 9.921 ms | 快 71.7% |

## 扩展矩阵

扩展测试覆盖 `16/32/64/256/1024/4096/65536B`、`1/2/4/8` 线程，共 28
种组合。每个线程执行 25000 次分配和释放，每个块实际写入首尾字节，每项运行
3 次取中位数。在本次 Windows 测试中，28 组 v3 均快于 `new/delete` 和
`malloc/free`。

| 尺寸 | 线程 | v3 | new/delete | malloc/free |
|---:|---:|---:|---:|---:|
| 16B | 1 | 0.400 ms | 1.101 ms | 1.012 ms |
| 32B | 8 | 0.777 ms | 2.512 ms | 2.578 ms |
| 256B | 8 | 0.744 ms | 3.502 ms | 3.313 ms |
| 1KB | 8 | 0.813 ms | 5.188 ms | 4.396 ms |
| 4KB | 8 | 1.149 ms | 5.959 ms | 4.696 ms |
| 64KB | 8 | 84.162 ms | 638.919 ms | 639.161 ms |

跨线程所有权转移测试由一个线程分配 50000 个 64B 块，另一个线程全部释放：

| v3 | new/delete | malloc/free |
|---:|---:|---:|
| 1.998 ms | 2.358 ms | 2.099 ms |

PageCache 最多缓存 64MB 已释放页，并继续执行相邻 span 合并；超过预算的完整
映射会归还操作系统。这避免了无上限驻留，同时消除重复 churn 中频繁
`VirtualAlloc`/`VirtualFree` 的主要开销。

## 五轮原始数据

| 场景 | 分配器 | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 |
|---|---|---:|---:|---:|---:|---:|
| 32B 小对象 | v3 | 3.276 | 2.320 | 2.322 | 2.306 | 2.404 |
| 32B 小对象 | new/delete | 6.048 | 5.082 | 5.029 | 4.879 | 5.210 |
| 32B 小对象 | malloc/free | 5.178 | 4.863 | 4.913 | 5.333 | 5.444 |
| 4 线程 | v3 | 3.043 | 1.865 | 2.418 | 2.120 | 1.820 |
| 4 线程 | new/delete | 5.366 | 5.052 | 4.526 | 4.702 | 4.756 |
| 4 线程 | malloc/free | 4.556 | 4.801 | 4.722 | 5.000 | 4.339 |
| 混合尺寸 | v3 | 6.045 | 2.812 | 2.954 | 2.805 | 2.714 |
| 混合尺寸 | new/delete | 10.029 | 9.782 | 10.040 | 9.602 | 10.017 |
| 混合尺寸 | malloc/free | 9.832 | 10.165 | 9.921 | 10.415 | 9.530 |

## 复现

```powershell
cmake -S v3 -B v3/build-release `
  -DKAMA_MEMORY_POOL_BUILD_TESTS=ON `
  -DKAMA_MEMORY_POOL_ENABLE_STATS=OFF `
  -DKAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS=OFF
cmake --build v3/build-release --config Release --parallel
.\v3\build-release\Release\perf_test.exe
```

统计和调试保护用于诊断，会增加热路径开销。生产性能构建默认关闭这两个选项；
需要 `MemoryPool::stats()` 返回计数时，应显式设置
`KAMA_MEMORY_POOL_ENABLE_STATS=ON`。

性能结果只代表上述机器和工作负载。商用评估还应在目标硬件上运行长期压力、
跨线程释放、真实对象尺寸分布和内存占用测试。
