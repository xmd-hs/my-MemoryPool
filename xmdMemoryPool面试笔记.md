<!-- markdownlint-disable MD013 MD024 MD026 MD033 MD036 MD040 MD060 -->

# xmdMemoryPool 面试笔记

基于 xmdMemoryPool（v1 → v2 → v3，含 `2855c59` 之后的 trim / 页标签 / Transfer）整理。
读完后应能独立讲清：为什么做池、三层怎么分、锁怎么减、跨线程块怎么回到可复用状态、unsized free 怎么做到接近 glibc，以及和 tcmalloc 的差距。

## 目录

1. 30 秒电梯稿
2. 先搞懂：malloc 到底慢在哪
3. 项目三版演进
4. 总架构：申请与释放两条路
5. Size Class：一切路由的起点
6. 第一层 ThreadCache
7. 第二层 CentralCache + Transfer Cache
8. 第三层 PageCache + 页标签 + 基数页表
9. 跨线程释放：TLS → Transfer
10. 无锁、ABA、内存序、自旋锁
11. 大对象、对齐、STL Allocator、trim
12. 平台层：mmap / VirtualAlloc
13. 性能怎么讲，和工业分配器差在哪
14. 面试题精讲
15. 白板怎么讲（3 分钟 / 8 分钟）

---

## 1. 30 秒电梯稿

**一句话：** 这是一个面向高频小对象的 C++17 内存池。小对象走「线程本地空闲链表 → 按 size class 分片的中央缓存（带 lock-free Transfer 杂志）→ 页缓存」三层；大对象走页缓存 + 本线程 8 MiB 上限的 span 复用；系统内存用 mmap / VirtualAlloc 按页拿。

解决的三件事：

| 痛点 | 池怎么解 |
|---|---|
| 每次 malloc 可能进锁、甚至进内核 | 热点路径只碰 TLS 空闲链表，无锁；下一块 prefetch |
| 多线程抢同一把全局锁 | CentralCache 按 size class × 8 shard，miss 后 steal，再 pop Transfer |
| 页级碎片、反复向 OS 要内存 | PageCache 缓存 span、同 origin 相邻合并，空闲上限 64 MiB |

**对外接口：** `MemoryPool::allocate/deallocate`、`allocateAligned`、`trim`、`newElement/deleteElement`、`Allocator<T>`、C ABI `xmd_alloc` / `xmd_free` / `xmd_trim`。

**性能口播**（提交 `2855c59`，关 Stats/Guards、Release + LTO/LTCG）：Windows 核心套件 48 WIN / 0 LOSE；Linux 46 WIN / 2 TIE / 0 LOSE。不要说「全面碾压 glibc」，说「核心套件不慢于系统 5%，Windows 领先幅度更大」。

---

## 2. 先搞懂：malloc 到底慢在哪

面试官问「为什么要自己做内存池」，不要只答「快」。要拆成四层成本：

```
应用 new / malloc
    |
    |-- 1) 函数调用 + 参数校验
    |-- 2) 分配器内部锁（ptmalloc 每个 arena 一把 mutex；Windows CRT 堆锁更重）
    |-- 3) 找合适的 bin / 切分 / 合并（算法本身）
    +-- 4) 堆上没内存 -> brk / mmap / VirtualAlloc（最贵）
```

高频小对象（几十字节、每秒百万次）里，② 和 ③ 才是主因：锁竞争 + 缓存行失效。④ 相对少，但一旦发生延迟是微秒～毫秒级（Windows 上 64KB × 8 线程系统堆可以到几百毫秒）。

**记忆口诀：** 池不是替代 OS，而是把「多次小申请」攒成「一次大申请」，再在用户态用空闲链表纳秒级命中。

常见对比：

| 方案 | 适用 | 缺点 |
|---|---|---|
| 直接 new/delete | 低频、大对象 | 锁、碎片、局部性差 |
| 固定槽对象池 | 单一类型、生命周期类似 | 不能混尺寸 |
| 本项目（size class + 三层） | 多尺寸、多线程、小对象为主 | 内部碎片；实现复杂 |
| tcmalloc / jemalloc / mimalloc | 生产级全局替换 | 工程量远大于教学项目 |

---

## 3. 项目三版演进

面试最爱问「你这个项目是怎么迭代的」。按 **问题 → 改法** 讲，不要按文件名讲。

### 3.1 v1：固定槽 + 无锁自由链表（教学版）

- 64 个池，槽位 8, 16, 24, …, 512 字节。
- 每个池内部：`operator new` 一次拿 4KB block，切成等大 Slot。
- 空闲槽用 **带 tag 的 CAS 无锁栈**（防 ABA）。
- 向系统要新 block 时才加 mutex，避免多线程重复 `operator new`。
- `> 512B` 直接 `operator new`。

**能讲的点：** 侵入式空闲链表、CAS、ABA、placement new。

**撑不住的点：** 所有线程抢同一条 `freeList_`；扩块一把 mutex，5 线程直接崩（池 736ms vs new 80ms）；没有页管理；没有批量；大于 512 完全不走池。

### 3.2 v2：三层雏形（tcmalloc 骨架）

引入 ThreadCache / CentralCache / PageCache，size class 扩到 256KB。

仍有的硬伤：

- 大对象走 malloc/free，和页缓存两套世界。
- 释放必须带 size（不知道指针属于哪个 class）。
- CentralCache 每个 size class 一把锁，线程一多就撞车。
- 跨线程释放会污染对方 ThreadCache（A 分配、B 释放，块进了 B 的 TLS），且没有 Transfer 杂志，线程一死切好的块只能拆回 span。

### 3.3 v3：可讲的「工程版」（推荐，以当前代码为准）

在 v2 骨架上补齐面试官会追问的每一刀：

| 能力 | v2 | 当前 v3 |
|---|---|---|
| 小对象上限 | 256KB | 512KB（覆盖 vector 几何扩容） |
| 大对象 | malloc | 统一走 PageCache span + 本线程 8 MiB 复用 |
| unsized free | 不支持 | 页尾 PageTag（≤256B）→ 三级 lock-free 页表 |
| Central 锁 | 每 class 1 把 | class × 8 shard，允许 steal |
| 跨线程 | 进当前线程 TLS，无线上杂志 | TLS + 有界 TransferBin；remote 栈仍保留 |
| 线程退出 | 有 flush | `flushAll` → `pushTransfer`，析构再冲大 span |
| 对齐 / STL / C API | 弱 | `Allocator<T>` + `xmd_*` + `trim()` |
| 可观测 | 无 | 可选 Stats（含 per-class hit/miss）/ Debug Guards / ASan TSan |
| 页缓存上限 | 无明确驱逐 | 空闲页上限 64 MiB，超出还 OS |
| 构建 | 无 | Release IPO（LTO / LTCG），sanitizer 时关闭 |

v3 内部还有一轮「为打过 glibc」的迭代，面试按时间线讲更加分：

1. 分片 + 提高自旋阈值，少 yield
2. remote free（防 TLS 污染）
3. 页标签 + 基数页表（unsized 不再先抢页锁）
4. Transfer cache（短命线程的块留给后来者，不必拆回 span）
5. 大 size class 留在 TLS / 页缓存，避免每次 mmap
6. 做过但否掉：bump-pointer 当前页、超大 transfer（RSS 167MiB）、Linux 16 片（短命线程狂 `createSpan`）

---

## 4. 总架构：申请与释放两条路

申请路径：

```
                    +-------------------------------------+
                    |      App / STL Allocator / C API    |
                    +------------------+------------------+
                                       |
                                       v
                             MemoryPool::allocate
                                       |
                 +---------------------+---------------------+
                 | size < 512KB        | large / over-aligned|
                 v                     v
          ThreadCache                  TLS large-span cache
                 | miss                      | miss
                 v                           v
          CentralCache.fetchRange      PageCache.allocateLargeSpan
                 |
                 |  (1) 本 shard 摘一批
                 |  (2) popTransfer
                 |  (3) steal 其他 shard
                 |  (4) mergeRemoteFrees
                 |  (5) createSpan
                 v
          PageCache.allocateSpan
          mmap / VirtualAlloc
```

- 左路：小对象，先 TLS，miss 再中央缓存，最后才向页缓存 / OS 要内存。
- 右路：大对象或过对齐，先看本线程 8 MiB span 缓存，没有再走 `allocateLargeSpan`。

释放路径：

```
deallocate(ptr, size?)
        |
        |-- 有 size 且 < 512KB --> SizeClass::getIndex
        |                              |
        |                              v
        |                       插入本线程 TLS
        |                              |
        |                    超水位 --> pushTransfer
        |                    Transfer 满 --> returnRange 回 span
        |
        +-- 无 size -------------> 先读页尾 PageTag (<=256B)
                                   miss --> 基数页表取 size class
                                   大对象 --> findLargeSpan
                                   然后同上（小对象进 TLS / 大对象进 TLS 大缓存）
```

- 有 size：直接算档位，进本线程 TLS。
- 无 size：先页尾标签，再基数页表；大对象走 `findLargeSpan`。
- 超水位或线程退出：块进 Transfer，满了再按 span 归还。

面试一句话：

- **快路径：** 本线程链表头删，几个指针 + prefetch 下一块。
- **慢路径：** 批量向中央要（先杂志、再分片、再页），最后才进内核。
- 释放不走「按欠账判断 remote」：当前热路径就是进本线程 TLS，靠 Transfer 在线程间搬家。

---

## 5. Size Class：一切路由的起点

### 5.1 为什么不能「要多少给多少」

若每种字节数一条链表，链表数量爆炸，中央锁也爆炸。工业分配器都把请求 **向上取整到有限档位**（size class）。

本项目常量：

- 对齐基准 `kAlignment = 8`
- 小对象上限 `kMaxBytes = 512KB`（≥ 512KB 走大对象路径）
- 表容量 `kMaxSizeClasses = 96`，实际档位数 = 编译期生成的 count

生成规则（编译期 `constexpr`）：

```
align = 8
反复: size += align
      记录这一档
      若 size >= align * 8，则 align *= 2
直到超过 512KB
```

前几档直观结果：

| 请求范围 | 落到的 class（示例） | 内部碎片 |
|---|---|---|
| 1–8 | 8 | 最多 7B |
| 9–16 | 16 | 最多 7B |
| 17–24 | 24 | … |
| … 直到 64 | 每 8B 一档 | 相对小 |
| 65–80 | 80 | 最多 15B |
| 129–160 | 160 | 最多 31B |
| 越大 | 步长按 2 倍涨 | 比例仍约 ≤ 12.5% |

这就是 tcmalloc 的「每个数量级 8 档」：步长 = 当前 align，连续约 8 档后 align 翻倍。碎片上限大约 1/8。

把上限从 256KB 扩到 512KB，是为了让 vector 几何扩容仍走 size-class + TLS，而不是每次大 span。

### 5.2 为什么用查表而不是除法

v3 对 `bytes ≤ 256` 用 `kSmallIndexMap[bytes]`，更大用 `kIndexMap[(bytes+7)/8]`。热路径是一次数组下标，没有循环、没有除法。

**Q：** Size class 查表放栈上算不行吗？

**A：** 可以，但分配是纳秒级热路径。`constexpr` 表在 `.rodata`，一次 load 搞定，还能 `static_assert` 保证首档是 8、末档是 512KB。

### 5.3 对齐向上取整的位运算

```cpp
(size + alignment - 1) & ~(alignment - 1)
```

前提：`alignment` 是 2 的幂。面试官常让现场写。

非法对齐（0 或非 2 的幂）直接返回 `nullptr`：`(alignment & (alignment - 1)) != 0`。

过对齐时：先把 size 取整到 alignment，再 `getIndex`；若该档 `size % alignment != 0`，则向更大档走，直到档位是 alignment 的倍数。页对齐的 span 起点保证 2 的幂对齐。

---

## 6. 第一层 ThreadCache

### 6.1 它是什么

每个线程一份，用 `thread_local` 单例：

```cpp
static ThreadCache* getInstance() {
    static thread_local ThreadCache instance;
    return &instance;
}
```

内部表：

- `freeList_[class]` / `freeListSize_[class]`：该档空闲块的侵入式链表
- `remoteBatch_[class]`：曾用于攒一批再 CAS 到 remote 栈（基础设施还在；当前 deallocate 走 TLS）

**侵入式链表：** 空闲块的前 `sizeof(void*)` 字节存 next 指针。分配出去后这块内存归用户；释放时再写回去。零额外 per-block 元数据、对 cache 友好。最小档 8B ≥ 一个指针。

当前 slot 没有 per-block header（`SizeClass::usesHeader` 恒 false），用户指针就是块起点。

### 6.2 分配快路径（无锁）

```
allocate(size)
  size==0 -> 当成 8
  size>=512KB -> 大对象
  index = SizeClass::getIndex(size)
  若 freeList_[index] 非空：
      头删一个，--size，prefetch 下一块
      return
  否则 fetchFromCentralCache：一次拿一批，自己留用一个，其余挂链表
```

没有 `localAllocBalance` 了。以前用「本线程欠中央多少块」判断这次 free 是不是自己的；现在释放一律进 TLS，设计更简单，靠 Transfer 处理线程间流动。

### 6.3 批量大小与水位

不能一次拿 1 个（锁太勤），也不能一次拿无限（撑爆 TLS、饿死别的线程）。

| 块大小 | 单次 batch（Windows/Linux） | TLS 缓存上限 |
|---|---:|---:|
| ≤ 32B | 512 | 4096 |
| ≤ 64B | 256 | 2048 |
| ≤ 256B | 128 | 1024 |
| ≤ 1KB | 32 | 256 |
| ≤ 4KB | 最多 32，且 ≥8 | 512 |
| ≤ 64KB | 最多 32 | 256（专门覆盖可扩展矩阵） |
| 更大 | ≤ 32 | ≤ 128 |

归还策略：超过水位时留下约一半，其余 `pushTransfer`。Transfer 满了才 `returnRange` 拆回 span。

macOS 批次更保守（小对象 256/128…），因为本机线程少、过度缓存会浪费 RSS。

### 6.4 线程退出与 trim

`~ThreadCache()`：`flushAll()` 把残留小对象 `pushTransfer`，再把本线程大 span 缓存还 PageCache。否则线程死了，这些页永远困在 TLS 里 → 泄漏。对应测试 `testThreadExitReturnsMemory`。

`MemoryPool::trim()` / `xmd_trim()`：空闲点主动把超水位小对象推走，并清空本线程大 span 缓存。cooperative，不扫描别的线程 TLS。对应 `testExplicitTrim`。

**Q：** `thread_local` 析构顺序会不会已经用不到 CentralCache？

**A：** CentralCache / PageCache 是进程级 Meyers 单例，析构在 atexit 阶段，通常晚于线程 TLS。`returnGroups` 刻意不用 `thread_local` 临时 vector，避免 TLS 析构顺序踩踏。真正危险的是 main 退出时仍有分离线程在分配——生产级分配器会更谨慎。

---

## 7. 第二层 CentralCache + Transfer Cache

### 7.1 角色

ThreadCache 的「批发商」：

- 按 size class 切 span，把一页（或多页）切成等大 block，串成链表
- 响应 `fetchRange` / `returnRange`
- 维护 TransferBin：已经切好、可直接给下一个线程的杂志
- 仍能吞 remote free（分片和杂志都空时）
- span 全部空闲后还给 PageCache

### 7.2 分片：把一把锁拆成很多把

```
shard 下标 = sizeClassIndex * 8 + shardId
全平台统一 8 shard（Linux 试过 16，短命线程更爱 createSpan，改回来）
shardId = hash(thread_id) % 8   // 线程绑死一个「主 shard」
```

每个 shard：

```cpp
struct CacheShard {
    alignas(64) std::atomic_flag lock;  // 独占一行，防 false sharing
    Span* partial;                      // 还有空闲块的 span 链表
};
```

`alignas(64)`：false sharing。若两个 shard 的 lock 挤在同一 cache line，A 线程 CAS lock 会把 B 的 line 作废，分片等于白做。

**fetchRange 顺序（面试必背）：**

1. 锁本线程主 shard，从 partial span 上摘一批
2. 没有则 `popTransfer`（无锁杂志，空仓一次 relaxed load，热路径不白查）
3. 再没有则 steal 其他 shard（仍只锁对方那一把）
4. 再没有且 `remotePending` 置位 → `mergeRemoteFrees`
5. 再没有则 `createSpan`，挂到主 shard，立刻再摘

**Q：** 为什么允许「借」别的 shard？

**A：** 否则某线程主 shard 空了，即使同类内存在别的 shard 堆着，也只能再向 OS 要页 → RSS 膨胀。借的时候锁的是对方 shard，不是全局锁。

**Q：** 为什么 Transfer 放在 steal 之前？

**A：** Transfer 里是现成块链表，不必再从 span 切。短命线程退出后，后来者应先吃杂志，而不是去锁别人的 partial 或 `createSpan`。这是打 glibc tcache「线程走了块还在」的关键。

### 7.3 TransferBin（有界 lock-free 杂志）

每个 size class 一个：

```cpp
struct alignas(64) TransferBin {
    atomic<void*> head;
    atomic<size_t> count;
};
```

- **push：** CAS 头插整链；超过 cap 的溢出走 `returnRange`
- **pop：** `exchange(nullptr)` 摘整条，留下超出本次 batch 的尾巴再 CAS 回去
- **cap** 按块大小：≤256B → 256 块；≤4KB → 64；更大 → 16

做过超大杂志（上万块）+ 放大 batch：RSS 顶到约 167MiB，unsized/vector LOSE，所以收小。口播：「杂志要有，但不能变成第二个页堆。」

### 7.4 Span 切块

`createSpan` 按块大小决定一次拿多少页（减少切块次数）：

| blockSize | 目标 span 字节 |
|---|---|
| ≤ 256B | 32 KB（每页末尾留 PageTag） |
| ≤ 1KB | 128 KB |
| ≤ 4KB | 512 KB |
| 更大 | 2 MB |

≤256B 时按页切：每页可用 = `pageSize - sizeof(PageTag)`，页尾写 `magic ⊕ 页地址` 和 sizeClass。这样 unsized free 和对象在同一页，O(1) 恢复档位。

大于 256B 仍整 span 连续切块，不写页标签（标签相对块太大，不划算）。

span 有一个 **不可变 home shard**：第一次挂上哪个 shard，之后归还必须加那把锁。否则两个 shard 同时改同一个 `span->freeList` 就是数据竞争。`centralShard` 用 atomic，未发布时是 `-1`。

### 7.5 还回去：按 span 归组

`returnRange` 先 `findSpan` 把指针按 `Span*` 分桶，再对每个 span 加 **它 home shard** 的锁，把头插回 `span->freeList`。

- `freeCount == blockCount` → 整 span 空了 → 从 partial 摘掉 → `PageCache::deallocateSpan`
- 原来 `freeCount==0`（完全分配出去的 span）这次有块回来 → 重新挂回 partial

分组时用相邻页号缓存 `lastSpan`，避免每个指针都走页表。

---

## 8. 第三层 PageCache + 页标签 + 基数页表

### 8.1 Span 是什么

连续若干页 + 元数据。不是用户可见的对象，是页层记账单位。

| 字段 | 含义 |
|---|---|
| `pageAddr` / `numPages` | 这段 span 覆盖的页 |
| `originAddr` / `originPages` | 当初 mmap 出来的整块（合并不能跨 origin） |
| `freeList` / `freeCount` / `blockCount` | 切块后的空闲情况 |
| `blocksPerPage` | 非 0 表示按页打包并带 PageTag |
| `sizeClass` / `isLarge` | 小对象档位 or 大对象 |
| `centralShard` | home shard（atomic） |
| `inFreeList` | 是否在 PageCache 空闲树里 |

### 8.2 分配：best-fit 切分

`freeSpans_` 是 `map<页数, Span*>`，`lower_bound(numPages)` → 找到 **不小于需求的最小档**（best-fit）。

若 span 比需要的大：切成「返回部分 + 剩余部分」，剩余按新页数挂回 map，并 `registerPages`。

没有合适的：`systemAlloc` → mmap/VirtualAlloc，粒度按系统 allocation granularity 向上取整（Windows 常 64KB）。

大对象走 `allocateLargeSpan`：从同样的空闲树拿，但登记到 `largeSpans_` 哈希，并从 pageMap 摘掉，避免 unsized 小对象路径误伤、也不走页表读锁。

### 8.3 释放：相邻合并 + 缓存上限

只和 **同一 origin 且 地址相邻 且 也在空闲链表里** 的 span 合并（先向后、再向前）。跨 origin 合并没有意义：不能把两次 mmap 当一块 munmap。

空闲页总量超过 64 MiB 且该 span 是完整 origin：直接 `releaseOrigin` 还 OS。这是防内存膨胀的硬顶。

还 OS 前会清掉 PageTag magic，避免虚地址复用后读到旧档位。

注意：不要缓存「页 → size class」到 TLS。同一虚页可能被 PageCache 回收后再切成别的档。页标签永远以页上那一份为准。

### 8.4 页尾 PageTag（unsized free 的快路径）

```cpp
struct PageTag { uint64_t magic; uint32_t sizeClass; uint32_t _pad; };
magic = 0x586D64506F6F6C31 ^ pageBase ^ (pageBase >> 17);  // "XmdPool1"
```

- 只给 `blockSize ≤ 256` 的页打标签
- 曾经固定留 64B，后来改成只留 `sizeof(PageTag)`，减少每页内部碎片
- `lookupSizeClass`：先 `pageTagIfValid`，失效再走页表

对标 glibc：glibc 的 chunk 在用户指针前面就有 size；我们不能破坏最小 8B 侵入式链表，所以把元数据放到页尾。面试就这么对比。

### 8.5 三级 lock-free 页表

```
page_id = addr >> pageShift
覆盖约 36 bit 页号（48-bit VA / 4KiB 页）：12 + 12 + 12
```

叶子槽打包：低 48 位 `Span*`，48..55 位存 `sizeClass+1`（0 表示未打标）。unsized 中大对象一次 atomic load 就能拿到档位。

读路径无锁；树长高时用 `growMutex_` 双检。叶子可 TLS 缓存（4K 页时一片叶子覆盖 16MiB）。

大对象不进这张页表，走 `largeSpans_` + mutex，所以小对象 unsized free 不会被大对象查找拖进 `PageCache::mutex_`。

**Q：** 以前为什么是 hashmap + `shared_mutex`？

**A：** 教学上够讲「页号 → span」。打 glibc 时 unsized 路径的读锁太贵，换成基数树。面试说到「读无锁、写很少、叶子打包 size class」加分。

---

## 9. 跨线程释放：TLS → Transfer（remote 是第二条回收集成）

这是最容易讲错、也最容易被追问的一节。按「现在代码怎么走」讲，再用「我们试过什么」作对比。

### 9.1 问题

生产者线程 A allocate，消费者线程 B deallocate（无锁队列、对象池、线程池任务结果都这样）。

另外还有：短命线程退出，TLS 里还有切好的块。glibc tcache 的优势就是「人走块留」；若每次都拆回 span 再切，新线程必慢。

### 9.2 当前规则（热路径）

本线程释放小对象：

```
一律插入本线程 TLS（freeSmall）
超水位 -> pushTransfer
Transfer 满 -> returnRange 回所属 span / home shard
```

线程退出：

```
整条 TLS 链 pushTransfer
残余 remoteBatch flush 到 remote 栈（若有）
大 span 缓存还 PageCache
```

取内存时后来者 `popTransfer`，不必知道块原来是谁切的。

这和早期设计不同：早期用 `localAllocBalance`，「自己的块」进 TLS，「别人的块」走 remote。后来发现：

- 打 glibc 短命线程，Transfer 比「立刻按 span 归组」更重要
- 释放进本线程 TLS 局部性更好，实现更短
- 页标签已经让 unsized 足够便宜，不必为了「不查表」强行带 size 走 balance

### 9.3 remote 栈还在，但不是 deallocate 主路径

每个 `(size class, shard)` 一个 `alignas(64)` atomic 头指针，CAS 头插。

`remotePending_[class]` 用 `exchange(0)` 清提示位，避免「读到 1、清 0、中间又有人 push」丢通知。

`fetchRange` 在分片和 Transfer 都 miss 时才 `mergeRemoteFrees`：摘整条链，按 span 归组，加 home shard 锁挂回。

`queueRemoteFree` / `remoteBatch` 仍在 ThreadCache 里（攒满 128/64 再一次 CAS），当前 deallocate 没有调用它。面试被问到要对着代码诚实：这是演进留下的回收集成，正确性测试（跨线程 unsized / 多消费者）现在走的是 TLS+Transfer。

### 9.4 和无锁栈 ABA

Remote / Transfer 都是 Treiber stack。ABA 经典场景：

```
线程1 读到 head=A，next=B
线程2 pop A，pop B，再把 A push 回去，此时 A->next 不再是 B
线程1 CAS 成功，head 变回 A 但链表已经坏了
```

v1 用 `TaggedPtr`（指针 + 版本号）显式防 ABA。v3 的 remote/transfer 没有 tagged pointer。面试要诚实：

- 64 位上常靠「用户空间指针高位未用」或「短窗口不容易 ABA」
- 正确做法是 tagged CAS / hazard pointer / epoch
- 这是和工业分配器的差距之一

Transfer 的 pop 用 `exchange` 摘整链，比单节点 CAS pop 窗口更短，但 push 仍是经典 CAS 头插。

---

## 10. 无锁、ABA、内存序、自旋锁

### 10.1 v1 的 CAS 自由链表（必会手撕）

入队（release）：先把 `slot->next = oldHead`，再 CAS 把头换成 slot，tag+1。

出队（acquire）：读 `oldHead->next` 作为新头，CAS，tag+1。

`compare_exchange_weak`：允许虚假失败，配合循环比 strong 在弱内存平台更便宜。

内存序口诀：

| 操作 | order | 为什么 |
|---|---|---|
| 成功入队 | release | 保证 next 的写入对弹出方可见 |
| 成功出队 | acquire | 看到入队方之前的写入 |
| 失败重试 | relaxed | 只是再读一遍头 |
| 改 next | relaxed | 由随后的 release CAS 发布 |

### 10.2 自旋锁

```cpp
while (flag.test_and_set(acquire)) {
    cpuPause();          // x86: PAUSE；ARM: yield
    // 超过阈值 then yield();
}
// 析构: flag.clear(release);
```

- **PAUSE：** 降低流水线争用、省电，不让出 CPU。
- 自旋太久才 yield：持锁时间极短（摘几个指针）时，yield 的上下文切换更亏。
- Apple 阈值 256、其他 1024：M 系核少、过度空转伤电。

**Q：** CentralCache 为什么不用 mutex？

**A：** 临界区是「从链表摘 N 个指针」，几十纳秒。mutex 可能陷入内核。自旋更合适。PageCache 临界区有 map 操作、可能 mmap，用 mutex 才对。页表读路径已经无锁，`PageCache::mutex_` 只管分配/释放/合并。

### 10.3 单例

`static CentralCache instance`（Meyers）：C++11 起静态局部初始化线程安全。不要手写 double-check，除非面试官要听 DCLP + atomic + `call_once`。

---

## 11. 大对象、对齐、STL Allocator、trim

### 11.1 大对象路径

`size ≥ 512KB`：先看本线程 `LocalLargeCache` 有没有同页数的 span；没有再 `allocateLargeSpan`。

缓存上限按 **保留字节** 而不是 span 个数：`kMaxCachedBytes = 8 MiB`。按个数会在反复用大 span 时把地址空间撑爆。

释放：先尝试 store 进 TLS 缓存；放不下再 `deallocateSpan`。完整 origin 且页缓存超 64 MiB 会 munmap。

不走 malloc 的原因：和页缓存统一、能 unsized free、能合并/复用页。

### 11.2 过对齐

默认池对齐 8。`alignof(T) > 8` 或 `allocateAligned(size, 16/32/…)`：把 size 向上取整到 alignment 的倍数，必要时升档直到 `size % alignment == 0`。

非法：`alignment=0` 或非 2 的幂 → `nullptr`。溢出：`size > SIZE_MAX - (align-1)` → `nullptr`。

### 11.3 newElement 异常安全

```cpp
void* mem = allocate(...);
try { return ::new (mem) T(args...); }
catch (...) { deallocate(mem); throw; }
```

构造抛异常必须把内存还回去，否则泄漏。`deleteElement`：先析构再 deallocate。对应 `testNewElementExceptionSafety`。

### 11.4 STL Allocator 协议（常考）

```cpp
using propagate_on_container_move_assignment = std::true_type;  // 容器 move 时带走分配器
using is_always_equal = std::true_type;                         // 任意两个 Allocator 可互换
// allocate 失败 -> throw bad_alloc                             // 池返回 nullptr 时转换
// n * sizeof(T) 溢出 -> throw bad_array_new_length
```

`operator==` 恒为 true：无状态分配器。这样 `vector<T, Allocator<T>>` 可以正常 swap / move。

deallocate 对 `alignof(T)≤8` 走 sized free，否则 unsized（过对齐升档后调用方手里的 `n*sizeof(T)` 不一定等于实际档位）。

### 11.5 trim

`MemoryPool::trim()` 只处理当前线程：超水位小对象推 Transfer，大 span 缓存清空。C ABI 是 `xmd_trim()`。

不要说「全局回收所有线程」——做不到，也不该在热路径扫别人的 TLS。

---

## 12. 平台层：mmap / VirtualAlloc

| 操作 | Linux / macOS | Windows |
|---|---|---|
| 分配 | `mmap(ANONYMOUS)` | `VirtualAlloc(MEM_RESERVE, COMMIT)` |
| 释放 | `munmap(ptr, size)` 必须带长度 | `VirtualFree(ptr, 0, MEM_RELEASE)` 不需要长度 |
| 页大小 | 通常 4KB（Apple 部分 16KB） | 4KB |
| 分配粒度 | 等于页大小 | 常 64KB |

所以 `systemAlloc` 会把字节数向上取整到 granularity。面试官问「为什么 Windows 一次最少很多页」就是这个。

为什么页层不用 malloc：

- malloc 出来的内存再切 span，无法按页对齐、无法 munmap 部分
- 会和系统分配器互相嵌套、死锁风险
- 页级分配器应当是最底层

Release 构建开 IPO（Linux LTO / MSVC LTCG），让 `MemoryPool::allocate` 有机会内联进用户 TU。开 ASan/TSan 时关掉，因为 sanitizer+LTO 脆，也不是性能配置。

---

## 13. 性能怎么讲，和工业分配器差在哪

### 13.1 怎么报数才不像造假

判定：每个场景对 `new/delete` 和 `malloc/free` 各比一次，取中位数。快于基线 5% 为 WIN，慢于 5% 为 LOSE，否则 TIE。核心套件出现 LOSE 则失败。

口径：提交 `2855c59`（2026-09-07），关 Stats/Guards。Windows 11 / MSVC Release+LTCG：**48 WIN / 0 TIE / 0 LOSE**。Linux Ubuntu 22.04 / GCC Release+LTO（WSL）：**46 WIN / 2 TIE / 0 LOSE**。

几个能背的锚点（单位 ms，越小越好）：

| 场景 | Windows 池 / malloc | Linux 池 / malloc |
|---|---|---|
| 32B sized ×100000 | 1.713 / 4.471 | 0.888 / 3.173 |
| 4 线程 sized ×100000 | 6.445 / 20.284 | 4.416 / 5.893 |
| 跨线程 unsized 64B | 1.275 / 1.801 | 1.085 / 2.848 |
| 8 线程 unsized burst | 5.705 / 10.195 | 3.647 / 3.873 |
| 64KB × 8 线程 | 1.611 / 970.413 | 1.919 / 41.168 |

Linux 两个 TIE：冷 512KB 对 malloc 0.001 vs 0.000；vector 对 new 约快 2.6%，未过 5% 线。

口播结论：

- **Windows：** CRT 堆锁重，主场景全面领先；64KB 复用把 VirtualAlloc 成本打掉两个数量级
- **Linux：** 同线程和混合尺寸稳赢；多线程短生命周期仍能 WIN，但领先幅度小于 Windows——glibc tcache 本身已经很快
- 不要用早期「Linux 4 线程打不过 glibc」的旧结论，那是加页标签 / Transfer / LTO 之前的数

### 13.2 和工业分配器差在哪

诚实讲差距，比吹「我写了个 tcmalloc」加分。

| 点 | 本项目 | tcmalloc / jemalloc / mimalloc |
|---|---|---|
| 结构 | TC / CC(+Transfer) / PC | 同思想；jemalloc 是 arena + bin；mimalloc 是 segment |
| Size class | 编译期表，上限 512KB | 更密、可运行时调 |
| Page map | 三级 radix + 页尾标签 | radix / 无锁；tcmalloc 还有采样 |
| 跨线程 | TLS + 有界 Transfer；remote 非热路径 | tcmalloc transfer cache + per-CPU；mimalloc 跨核 free list |
| ABA | v1 有 tag；v3 栈没有 | hazard / tagged / 不复用地址 |
| 安全 | 可选 debug map | 隔离、guard page、抽样检测 |
| 替换 malloc | 不挂钩 | alias / LD_PRELOAD |
| 大页 / NUMA | 无 | 有 |

默认关 Guards 时，错误 size / double free 仍可能破坏内部链表——生产环境要开检查或根本不让用户直接碰池内部。

### 13.3 明确做过但没留下的方案

| 方案 | 想解决 | 实际 |
|---|---|---|
| bump-pointer「当前页」 | 少碰自由链表 | 收益接近 0 |
| PageCache 按地址范围加锁 | 减页表锁 | 无收益或回归（后来直接 radix 无锁读） |
| 超大 Transfer + 放大 batch | 模仿 glibc arena | 167MiB RSS，unsized/vector LOSE |
| Linux 16 分片 | 减锁碰撞 | 短命线程更爱 `createSpan` |

原则：测的是系统分配器，不是自己跟自己比。LOSE 就改或撤。

---

## 14. 面试题精讲

按出现频率排。每题先给 **口播答案**，再给 **追问**。

### A. 基础认知

#### A1. 内存池是什么？和对象池、slab 什么关系？

**口播：** 内存池是预先（或按需成批）向 OS 拿大块内存，在用户态按固定或分档尺寸切分、用空闲链表管理的分配器。对象池通常还管构造/析构、只服务一种类型。Slab（Linux 内核）按对象类型分缓存，思想同源。本项目是 **多 size class 的通用小对象池**，不是单类型对象池。

**追问：** 内部碎片 vs 外部碎片？

内部：向上取整浪费（本项目有；≤256B 页还要再扣一个 PageTag）。外部：空闲块散落凑不出连续大块（页合并就是在打这个）。

#### A2. new / malloc / mmap 的关系？

```
new      -> operator new -> malloc -> (堆不够) brk 或 mmap
本池小对象 -> ThreadCache -> CentralCache -> PageCache -> mmap/VirtualAlloc
本池大对象 -> TLS 大缓存 或 PageCache -> mmap
```

new 会调构造；malloc 只给生内存。池的 allocate 等于 malloc；`newElement` 等于 malloc + placement new。

#### A3. 为什么 8 字节对齐？

64 位 ABI 要求 malloc 返回 16 字节对齐（System V），本项目选 8 是教学简化：double / 指针够用。`alignof(T)>8` 走 `allocateAligned`。面试可补一句「严格对齐 SIMD 类型要走对齐接口」。

### B. 设计题

#### B1. 为什么三层，两层行不行？

- **两层（只有 TLS + mmap）：** 线程退出或缓存爆了没法共享；页无法在线程间复用。
- **两层（只有中央 + 页）：** 每次分配都抢中央锁，回到 ptmalloc 的问题。
- **三层：** TLS 吸收热点；中央负责跨线程共享、切块、Transfer；页负责和 OS 打交道。每一层摊销下一层的成本。

#### B2. 批量大小怎么定？

权衡三点：锁次数、TLS 占用、公平性。

经验公式：`min(最大字节预算 / size, 该档经验上限)`。小对象多拿（锁摊得薄），大对象少拿（占内存）。水位高于阈值还一半到 Transfer，避免「饿死中央、撑死 TLS」。

#### B3. 如何支持「只给指针、不给 size 的 free」？

必须能从指针反查元数据。当前两条路：

1. ≤256B 页：页尾 PageTag，和对象同一 cache 友好页
2. 其他小对象：`page_id` → 基数页表叶子，一次 atomic load 解出 size class（打包在指针高位）

大对象：`largeSpans_` 哈希，必须是 span 起点。

代价：unsized 比 sized 多一次标签或页表 load。所以仍提供 `deallocate(ptr, size)`：小对象直接 `getIndex`，不碰页表。

#### B4. 如何处理 A 分配 B 释放？

当前：块进 B 的 TLS。B 退出或超水位 → TransferBin，A 或其他线程下次 fetch 时 pop 走。不要再说「用 `localAllocBalance` 判断后 `pushRemoteFree`」——那是旧热路径。

#### B5. 线程本地缓存会不会泄漏？

析构 `flushAll` + 大缓存 flush。进程退出 PageCache 析构再 munmap 全部 origin。空闲时可 trim。Debug 下可用 stats 的 `liveAllocs` 看是否配对。

#### B6. Transfer 和 remote 有什么区别？

- **Transfer：** 已经是「可直接再分配」的块链，跨线程杂志，有 cap。
- **Remote：** 按 span 延迟归组，最终回到 home shard 的 freeList，可能让整 span 空闲还页。

热路径优先 Transfer；remote 是更彻底的回收，但更贵。

### C. 并发与内存模型

#### C1. 什么是 ABA？你怎么防的？

见 10.1。v1：指针打 tag。v3 remote/transfer：目前依赖「实践上少见」，这是已知简化。可以补：64 位 CAS 把计数器放高位、或 hazard pointer。

#### C2. `memory_order_relaxed` 能当无锁队列用吗？

不能当发布手段。无锁链表：修改 next 可以用 relaxed，但 CAS 把头发布出去必须 release，另一端必须 acquire，否则可能看到 next 是野值。

#### C3. 自旋锁和 mutex 怎么选？

临界区 < ~1μs、不睡觉、核不爆炸 → 自旋。可能 mmap、可能等 IO、临界区有系统调用 → mutex。PageCache 用 mutex，CentralCache 用自旋，就是这个划分。页表读无锁，不占这把 mutex。

#### C4. false sharing 是什么？本项目哪里防了？

两个无关原子变量落在同一 64B cache line，一方写入使另一方的 line 失效。`CacheShard::lock`、`RemoteFreeList`、`TransferBin` 都 `alignas(64)`。

#### C5. `thread_local` 和 `__thread`？

C++11 `thread_local` 支持类类型、会调构造析构；GCC `__thread` 偏 POD。本项目 ThreadCache 有析构（要 flush），必须用 `thread_local`。

#### C6. `remotePending` 为什么要用 exchange 而不是 load+store？

清 hint 必须原子。若 load 看到 1、store 0，中间并发 push 把 hint 设回 1 会被覆盖丢失，merge 可能永远看不到新链。`exchange(0, acq_rel)` 取出旧值并清零，发布方随后 `store(1)` 不会丢。

### D. 实现细节（对着代码问）

#### D1. 空闲链表为什么写在用户内存里？

块空闲时用户不用这块字节。省掉并行的 meta 数组，分配后自然覆盖。代价：必须保证块 ≥ `sizeof(void*)`（本项目最小 8B）。Debug 下另用 `debugAllocMap` 记已分配，因为用户内存里不再有元数据。

#### D2. `curSlot_ += SlotSize_ / sizeof(Slot)`（v1）是什么意思？

`curSlot_` 类型是 `Slot*`，指针算术按 `sizeof(Slot)` 走。实际槽长是 `SlotSize_` 字节，所以要换算成「几个 Slot 宽度」。这是面试官爱抓的指针算术题。

#### D3. 合并 span 为什么必须同一 origin？

munmap 必须对当初 mmap 的范围。两次映射中间可能有空洞，或 Windows 上必须整块 `MEM_RELEASE`。`originAddr` 就是记账。

#### D4. `newElement` 构造失败呢？

catch 后 deallocate 再 throw。对应测试 `testNewElementExceptionSafety`：stats 上 `liveAllocs` 不变。

#### D5. Allocator 为什么 `is_always_equal`？

池是进程全局单例，分配器对象无状态。容器认为任何 `Allocator<T>` / `Allocator<U>` 都相等，move 后仍能用对方指针去 deallocate。

#### D6. Debug Guards 查什么？

- double free：`debugAllocMap[i]` 已是 0
- 内部指针：offset 不能整除 `blockSize`（有 PageTag 的页还要检查页内偏移 < usable）
- 野指针：`findSpan` 失败则忽略
- 大对象：必须整 span 起点

生产环境关掉（CMake `XMD_MEMORY_POOL_ENABLE_DEBUG_GUARDS`），热路径零开销（`if constexpr`）。

#### D7. 为什么 unsized 不缓存「页 → size class」？

PageCache 回收虚页后可能切成别的档。页标签 magic 绑的是当前页地址 cookie，失效会回退页表。TLS 缓存页号会读到错档，破坏链表。

#### D8. 大对象为什么不进 pageMap？

小对象 unsized 是热路径，不应为了大对象去抢 PageCache 锁。大对象用 `largeSpans_` 单独查，且必须整段起点。

### E. 对比与开放题

#### E1. 和 tcmalloc 比，你抄了什么、砍了什么？

- **抄：** size class、线程缓存、中央批量、页堆、span、transfer cache、radix page map。
- **砍：** 全局 malloc 替换、采样 profiler、TCMalloc 的 per-CPU 缓存（比 TLS 更现代）、安全释放延迟、大页/NUMA。
- **自研/裁剪：** 页尾 size-class 标签、8 分片 steal、64MiB 页缓存顶、8MiB 大对象 TLS 顶、C/STL/trim API、sanitizer 开关。

#### E2. 什么场景不该用这个池？

- 分配尺寸巨大且稀疏（直接 mmap 更干净）
- 需要和系统 free 混用（绝对不行）
- 实时系统要硬上限延迟（自旋 + mmap 无上界）
- 已有 jemalloc 且负载已经很快
- 需要检测 double-free / 越界的生产路径（默认 Guards 关着）

#### E3. 如何证明没泄漏、没数据竞争？

- 单元测试：跨线程 free、线程退出、异常安全、trim、页复用后再 unsized、`liveAllocs` 闭合
- ASan：越界、UAF
- TSan：Central 分片锁、页表增长锁、Transfer/remote CAS
- 不能 ASan+TSan 一起开（CMake 里直接 FATAL）

#### E4. 性能数字怎么讲才不像造假？

带平台、关 stats/guards、多次中位数、WIN 阈值 5%。结论应当是：

- Windows 小对象 / 混合 / 多线程：池明显快
- Linux 同线程和混合快；多线程能 WIN 但领先薄
- 64KB 档靠 TLS/页缓存复用，系统堆会去 VirtualAlloc/mmap，差距可以到两个数量级

### F. 手撕 / 现场写

面试官可能让写的最小集合：

#### F1. 无锁栈 push/pop（带 tag）

直接默写 v1。

#### F2. size class 向上取整

```cpp
index = getIndex(n);
return table[index];     // >= n 的最小档
// 大对象: (n + 7) & ~7
```

#### F3. 侵入式链表头插头删

```cpp
// alloc
p = head; head = *p; return p;
// free
*p = head; head = p;
```

#### F4. 对齐检测与向上取整

```cpp
is_pow2 = x && !(x & (x - 1));
align_up = (n + a - 1) & ~(a - 1);
```

#### F5. placement new 的对象池

```cpp
mem = pool.alloc(sizeof(T));
try { return new (mem) T(args...); }
catch (...) { pool.free(mem); throw; }
```

#### F6. 页号与标签

```cpp
page = ptr & ~(pageSize - 1);
tag  = page + pageSize - sizeof(PageTag);
page_id = ptr >> pageShift;
```

---

## 15. 白板怎么讲（3 分钟 / 8 分钟）

### 3 分钟版（自我介绍里带过）

我做了一个仿 tcmalloc 的三层内存池。小对象先走线程本地空闲链表，miss 了按 size class 从中央缓存批量拿；中央按 class 和线程哈希分成 8 片，miss 后先吃 Transfer 杂志再 steal。页层用 mmap 管 span，同 origin 相邻合并，空闲上限 64MB。unsized free 小对象先看页尾标签，再走无锁基数页表。大对象和对齐分配走同一套页缓存。Windows 上对 CRT 全面领先，Linux 核心套件不慢于 glibc 5%。

### 8 分钟版（项目深挖）

1. **动机（1 min）：** malloc 锁 + 系统调用；Windows 堆锁更痛；目标是同线程小对象无锁。
2. **画三层 + 两条路径（2 min）：** 小对象 / 大对象；allocate 向下、deallocate 向上进 TLS。
3. **Size class（20s）：** 每数量级约 8 档，查表，上限 512KB。
4. **并发（2 min）：** TLS 无锁 → Central 自旋+8 分片+steal → Transfer 无锁杂志 → Page mutex；页表读无锁。
5. **unsized（1 min）：** PageTag + radix 打包 size class。不要缓存页到档位。
6. **正确性（30s）：** home shard、同 origin 合并、线程退出 Transfer、构造异常还内存、TSan。
7. **数据与局限（30s）：** 48/46 WIN；ABA 简化；不挂钩 malloc；Linux 领先薄。

### 可能被打断时的「锚点句」

| 被问到 | 回一句 |
|---|---|
| 锁在哪 | 热路径无锁；中央每 shard 自旋；页分配一把 mutex；页表读无锁 |
| 怎么无锁 | TLS 单线程独享；Transfer/remote 用 CAS 栈；页表 atomic load |
| 块怎么跨线程 | 释放进当前 TLS，溢出/退出进 Transfer，后来者 pop |
| 内存还给 OS 吗 | 完整 origin 且超过 64MB 会 munmap；trim 只收当前线程缓存 |
| 和 v1 比 | v1 是单层无锁链表教学；v3 才是多线程可扩展结构 |
| 有什么 bug 你修过 | 16 片导致短命线程狂 `createSpan`；超大 Transfer 打爆 RSS；页→档位 TLS 缓存在页复用后读错 class；remote hint 用 store 会丢通知；大对象按个数缓存会撑地址空间 |
| Linux 打得过 glibc 吗 | 核心套件不 LOSE；领先幅度小于 Windows；靠的是页标签、Transfer、LTO，不是「算法全面更优」 |

---

## 附录 A. 文件地图（v3）

```
v3/
|-- include/xmd/      对外：MemoryPool / Allocator / C API / export
|-- src/
|   |-- Common.h       SizeClass 表、SpinLock、Stats 开关、prefetch
|   |-- ThreadCache.*  TLS 空闲链表、batch、大对象 8MiB 缓存、trim
|   |-- CentralCache.* 8 分片、切 span、TransferBin、remote 栈
|   |-- PageCache.*    span、PageTag、三级 PageMap、合并、mmap
|   |-- Platform.h     页大小、mmap / VirtualAlloc
|   `-- MemoryPool.cpp 门面 + 统计
`-- tests/             正确性 / 跨线程 / 异常 / trim / sanitizer / perf_test
```

## 附录 B. 学习检查清单

能在不问文档的情况下答出以下问题，就算过关：

- [ ] malloc 慢的四层成本
- [ ] 为什么要 size class，碎片上限大概多少，为什么上限是 512KB 不是 256KB
- [ ] 三层各自解决什么，少一层会怎样
- [ ] TLS 快路径几个指针操作，何时批量，溢出去哪
- [ ] 为什么 shard 要 `alignas(64)`，为什么是 8 不是 16
- [ ] home shard 不绑定会怎样
- [ ] Transfer 和 remote 的分工，当前热路径走哪条
- [ ] unsized free：PageTag 与基数页表谁先谁后，为什么不缓存页→档位
- [ ] ABA 是什么，v1 怎么防
- [ ] acquire/release 在 CAS 链表里谁用
- [ ] 自旋 vs mutex 在本项目怎么分工
- [ ] span 合并为何不能跨 origin
- [ ] 大对象 TLS 为什么按字节上限而不是个数
- [ ] `newElement` 异常安全
- [ ] STL allocator 三个 typedef 为什么那么写
- [ ] trim 能做什么、不能做什么
- [ ] Windows / Linux 性能怎么报，两个 TIE 是什么
- [ ] 和 tcmalloc 的真实差距
- [ ] 否掉过哪些方案（超大 Transfer、16 片、bump-pointer）

文档对应代码：xmdMemoryPool v1 / v2 / v3。面试以当前 v3 源码为准；用 v1 讲 CAS/ABA；用 v2→v3 和 [优化历程.md](优化历程.md) 讲「怎么从打不过 glibc 打到核心套件不 LOSE」。

性能数字以提交 `2855c59` 的 README / 优化历程第 8 节为准，后续改动（trim、页标签改小、大对象字节上限、remote hint exchange）没有改 API 语义。
