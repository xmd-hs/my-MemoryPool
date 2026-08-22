# Windows 性能数据

MSVC Release，stats/debug guards 关闭：

| 场景 | v3 | new/delete | malloc/free |
|---|---:|---:|---:|
| 32B 小对象，100000 次 | 2.315 ms | 5.441 ms | 5.013 ms |
| 4 线程，每线程 25000 次 | 1.949 ms | 4.976 ms | 5.096 ms |
| 16B-2048B 混合尺寸，50000 次 | 2.819 ms | 10.652 ms | 10.536 ms |
| 跨线程所有权转移，64B x 50000 | 2.129 ms | 2.587 ms | 2.313 ms |

数据来自当前 `v3/tests/PerformanceTest.cpp` 的 Release 实测，单位为毫秒，中位数。
