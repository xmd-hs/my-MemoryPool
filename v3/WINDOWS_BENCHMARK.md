# v3 Windows 性能测试

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
| 32B 小对象，100000 次 | 3.464 ms | 4.927 ms | 4.880 ms | 快 29.0% |
| 4 线程，每线程 25000 次 | 3.189 ms | 4.633 ms | 4.431 ms | 快 28.0% |
| 16B-2048B 混合尺寸，50000 次 | 8.548 ms | 9.875 ms | 9.560 ms | 快 10.6% |

## 五轮原始数据

| 场景 | 分配器 | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 |
|---|---|---:|---:|---:|---:|---:|
| 32B 小对象 | v3 | 3.475 | 3.464 | 3.630 | 3.240 | 3.254 |
| 32B 小对象 | new/delete | 5.092 | 4.780 | 4.927 | 4.811 | 5.005 |
| 32B 小对象 | malloc/free | 4.880 | 4.733 | 4.951 | 4.882 | 4.503 |
| 4 线程 | v3 | 3.993 | 3.221 | 2.683 | 3.094 | 3.189 |
| 4 线程 | new/delete | 4.978 | 4.334 | 4.633 | 4.710 | 4.497 |
| 4 线程 | malloc/free | 4.431 | 4.222 | 4.858 | 4.563 | 4.299 |
| 混合尺寸 | v3 | 9.094 | 8.179 | 8.253 | 8.548 | 8.689 |
| 混合尺寸 | new/delete | 9.875 | 9.076 | 9.665 | 10.072 | 10.084 |
| 混合尺寸 | malloc/free | 9.359 | 9.560 | 9.523 | 10.289 | 9.599 |

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
