// PageCache 的实现文件（第三版）
// 第三层，全局唯一：向系统要页，管的是一段段连续的页（Span）
// freeSpans_：按页数分桶的空闲链；allocate 找第一个 ≥ 需要页数的，大了就拆
// pageMap_：小对象页号 → Span*；largeSpans_：大对象起始地址 → Span*
// 归还只合并当初同一次向系统要来的相邻空闲段；完整空闲 origin 可按预算归还系统

#include "PageCache.h"                   // 包含页缓存头文件

#include <algorithm>                     // 包含 find_if
#include <unordered_set>                 // 包含去重用的 set

namespace Kama_memoryPool                // 进入 Kama_memoryPool 命名空间
{                                        // 命名空间开始

// ===== PageMap（头文件里有声明） =====

PageMap::~PageMap()                      // 析构：拆掉整棵三级表
{                                        // 函数开始
    for (size_t r = 0; r < kSize; ++r)   // 遍历第一级每一个根槽
    {                                    // 循环开始
        Node* mid = root_[r].exchange(nullptr, std::memory_order_relaxed); // 取出并清空这个中间节点
        if (!mid)                        // 如果这一槽本来就是空的
            continue;                    // 看下一槽
        for (size_t m = 0; m < kSize; ++m) // 遍历这个中间节点下的每一片叶子
        {                                // 内层循环开始
            auto* leaf = static_cast<Leaf*>(mid->slots[m].exchange(nullptr, std::memory_order_relaxed)); // 取出叶子
            delete leaf;                 // 释放叶子（空指针 delete 也安全）
        }                                // 内层循环结束
        delete mid;                      // 释放这个中间节点
    }                                    // 循环结束
}                                        // 函数结束

PageMap::Node* PageMap::getOrCreateNode(std::atomic<void*>& slot) // 槽里没有中间节点就新建
{                                        // 函数开始
    void* existing = slot.load(std::memory_order_acquire); // 先无锁看一眼
    if (existing)                        // 如果已经有了
        return static_cast<Node*>(existing); // 直接用
    std::lock_guard<std::mutex> lock(growMutex_); // 没有则加扩表锁
    existing = slot.load(std::memory_order_relaxed); // 锁内再看一次（别人可能刚建好）
    if (existing)                        // 如果锁内发现已经有了
        return static_cast<Node*>(existing); // 用别人建的
    Node* node = new Node();             // 新建中间节点
    slot.store(node, std::memory_order_release); // 发布到槽里
    return node;                         // 返回新节点
}                                        // 函数结束

PageMap::Leaf* PageMap::getOrCreateLeaf(std::atomic<void*>& slot) // 槽里没有叶子就新建
{                                        // 函数开始
    void* existing = slot.load(std::memory_order_acquire); // 先无锁看一眼
    if (existing)                        // 如果已经有了
        return static_cast<Leaf*>(existing); // 直接用
    std::lock_guard<std::mutex> lock(growMutex_); // 没有则加扩表锁
    existing = slot.load(std::memory_order_relaxed); // 锁内再看一次
    if (existing)                        // 如果锁内发现已经有了
        return static_cast<Leaf*>(existing); // 用别人建的
    Leaf* leaf = new Leaf();             // 新建叶子
    slot.store(leaf, std::memory_order_release); // 发布到槽里
    return leaf;                         // 返回新叶子
}                                        // 函数结束

void PageMap::set(uintptr_t pageId, Span* span) // 登记这个页号属于这段 span
{                                        // 函数开始
    const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1); // 第一级下标：页号最高 12 位
    const uintptr_t midIndex = (pageId >> kBits) & (kSize - 1); // 第二级下标：中间 12 位
    const uintptr_t leafIndex = pageId & (kSize - 1); // 叶子下标：最低 12 位
    Node* mid = root_[rootIndex].load(std::memory_order_acquire); // 取出第一级上的中间节点
    if (!mid)                            // 如果第一级还是空的
    {                                    // 创建第一级分支开始
        std::lock_guard<std::mutex> lock(growMutex_); // 加扩表锁
        mid = root_[rootIndex].load(std::memory_order_relaxed); // 锁内再读
        if (!mid)                        // 如果仍然没有
        {                                // 新建分支开始
            mid = new Node();            // 新建中间节点
            root_[rootIndex].store(mid, std::memory_order_release); // 挂到第一级
        }                                // 新建分支结束
    }                                    // 创建第一级分支结束
    Leaf* leaf = getOrCreateLeaf(mid->slots[midIndex]); // 拿到或创建对应叶子
    leaf->slots[leafIndex].store(pack(span), std::memory_order_release); // 写入打包后的 Span* 和档位
}                                        // 函数结束

void PageMap::clear(uintptr_t pageId)    // 清掉这个页号的登记
{                                        // 函数开始
    const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1); // 第一级下标
    const uintptr_t midIndex = (pageId >> kBits) & (kSize - 1); // 第二级下标
    const uintptr_t leafIndex = pageId & (kSize - 1); // 叶子下标
    Node* mid = root_[rootIndex].load(std::memory_order_acquire); // 取中间节点
    if (!mid)                            // 如果这一级不存在
        return;                          // 本来就没登记
    auto* leaf = static_cast<Leaf*>(mid->slots[midIndex].load(std::memory_order_acquire)); // 取叶子
    if (!leaf)                           // 如果叶子不存在
        return;                          // 本来就没登记
    leaf->slots[leafIndex].store(0, std::memory_order_release); // 写成未映射
}                                        // 函数结束

uintptr_t PageMap::loadPacked(uintptr_t pageId) const // 按页号读打包值（无叶子缓存）
{                                        // 函数开始
    const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1); // 第一级下标
    const uintptr_t midIndex = (pageId >> kBits) & (kSize - 1); // 第二级下标
    const uintptr_t leafIndex = pageId & (kSize - 1); // 叶子下标
    Node* mid = root_[rootIndex].load(std::memory_order_relaxed); // 取中间节点
    if (!mid)                            // 如果没有
        return 0;                        // 未映射
    auto* leaf = static_cast<Leaf*>(mid->slots[midIndex].load(std::memory_order_relaxed)); // 取叶子
    if (!leaf)                           // 如果没有
        return 0;                        // 未映射
    return leaf->slots[leafIndex].load(std::memory_order_relaxed); // 返回打包值
}                                        // 函数结束

uintptr_t PageMap::loadPackedCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const // 带线程本地叶子缓存读打包值
{                                        // 函数开始
    const uintptr_t leafId = pageId >> kBits; // 同一片叶子覆盖的页号前缀
    const uintptr_t leafIndex = pageId & (kSize - 1); // 叶子内下标
    Leaf* leaf = nullptr;                // 准备指向叶子
    if (cachedLeaf && cachedLeafId == leafId) // 如果缓存的正是这一片叶子
    {                                    // 命中分支开始
        leaf = static_cast<Leaf*>(cachedLeaf); // 直接用缓存
    }                                    // 命中分支结束
    else                                 // 缓存不是这一片
    {                                    // 未命中分支开始
        const uintptr_t rootIndex = (pageId >> (2 * kBits)) & (kSize - 1); // 第一级下标
        const uintptr_t midIndex = leafId & (kSize - 1); // 第二级下标
        Node* mid = root_[rootIndex].load(std::memory_order_relaxed); // 取中间节点
        if (!mid)                        // 如果没有中间节点
        {                                // 空表分支开始
            cachedLeaf = nullptr;        // 缓存记成空
            cachedLeafId = leafId;       // 但仍记下这次的叶子编号，避免反复走
            return 0;                    // 未映射
        }                                // 空表分支结束
        leaf = static_cast<Leaf*>(mid->slots[midIndex].load(std::memory_order_relaxed)); // 取叶子
        cachedLeaf = leaf;               // 更新线程本地叶子缓存
        cachedLeafId = leafId;           // 记下这片叶子的编号
        if (!leaf)                       // 如果叶子不存在
            return 0;                    // 未映射
    }                                    // 未命中分支结束
    return leaf->slots[leafIndex].load(std::memory_order_relaxed); // 从叶子读打包值
}                                        // 函数结束

Span* PageMap::get(uintptr_t pageId) const // 查出这个页号的 span
{                                        // 函数开始
    return unpackSpan(loadPacked(pageId)); // 读打包值再解出指针
}                                        // 函数结束

Span* PageMap::getCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const // 带缓存查 span
{                                        // 函数开始
    return unpackSpan(loadPackedCached(pageId, cachedLeaf, cachedLeafId)); // 带缓存读再解包
}                                        // 函数结束

size_t PageMap::getSizeClass(uintptr_t pageId) const // 查出这个页号的档位
{                                        // 函数开始
    const uintptr_t bits = loadPacked(pageId); // 读出打包值
    if (bits == 0)                       // 如果未映射
        return FREE_LIST_SIZE;           // 当作没有合法档
    const size_t sc = unpackSizeClass(bits); // 先看高位有没有打包档位
    if (sc < FREE_LIST_SIZE)             // 如果高位有档
        return sc;                       // 直接返回
    Span* span = unpackSpan(bits);       // 否则从 Span 身上再读
    if (!span || span->isLarge)          // 如果没有 span 或是大对象
        return FREE_LIST_SIZE;           // 没有小对象档
    return span->sizeClass;              // 返回 span 上记的档
}                                        // 函数结束

size_t PageMap::getSizeClassCached(uintptr_t pageId, void*& cachedLeaf, uintptr_t& cachedLeafId) const // 带缓存查档位
{                                        // 函数开始
    const uintptr_t bits = loadPackedCached(pageId, cachedLeaf, cachedLeafId); // 带缓存读打包值
    if (bits == 0)                       // 如果未映射
        return FREE_LIST_SIZE;           // 没有合法档
    const size_t sc = unpackSizeClass(bits); // 先看高位
    if (sc < FREE_LIST_SIZE)             // 如果高位有档
        return sc;                       // 直接返回
    Span* span = unpackSpan(bits);       // 否则问 span
    if (!span || span->isLarge)          // 如果没有或是大对象
        return FREE_LIST_SIZE;           // 没有小对象档
    return span->sizeClass;              // 返回 span 上的档
}                                        // 函数结束

// ===== PageCache =====

PageCache::~PageCache()                  // 析构：删掉所有 Span，把系统映射还回去
{                                        // 函数开始
    std::lock_guard<std::mutex> lock(mutex_); // 锁住页缓存
    std::unordered_set<Span*> spans;     // 用来给 Span* 去重
    for (auto& kv : largeSpans_)         // 遍历大对象表
        spans.insert(kv.second);         // 收进集合
    largeSpans_.clear();                 // 清空大对象表
    for (auto& kv : freeSpans_)          // 遍历每个页数桶
    {                                    // 桶循环开始
        for (Span* span = kv.second; span; span = span->next) // 走这条空闲链
            spans.insert(span);          // 收进集合
    }                                    // 桶循环结束
    freeSpans_.clear();                  // 清空空闲桶
    for (Span* span : spans)             // 每个 Span 只删一次
        delete span;                     // 释放元数据对象（不是虚存本身）
    for (auto& alloc : systemAllocs_)    // 遍历每一次向系统的申请
        freePages(alloc.first, alloc.second); // 把这段虚还回去
    systemAllocs_.clear();               // 清空申请记录
}                                        // 函数结束

uintptr_t PageCache::pageId(void* ptr) const // 把指针换成页号
{                                        // 函数开始
    return reinterpret_cast<uintptr_t>(ptr) >> systemPageShift(); // 地址右移页内位数
}                                        // 函数结束

void PageCache::registerPages(Span* span) // 把这段的每一页都写进页表
{                                        // 函数开始
    const size_t ps = systemPageSize();  // 取出页大小
    auto id = reinterpret_cast<uintptr_t>(span->pageAddr) / ps; // 起始页号
    for (size_t i = 0; i < span->numPages; ++i) // 从第 0 页到最后一页
        pageMap_.set(id + i, span);      // 这一页指向这段 span
}                                        // 函数结束

void PageCache::refreshPageTags(Span* span) // 档位定了之后重新登记（刷新打包档位）
{                                        // 函数开始
    if (!span || span->isLarge)          // 如果没有 span 或是大对象
        return;                          // 大对象不进页表
    registerPages(span);                 // 再 set 一遍，pack 会带上现在的 sizeClass
}                                        // 函数结束

void PageCache::unregisterPages(Span* span) // 把这段的每一页从页表清掉
{                                        // 函数开始
    const size_t ps = systemPageSize();  // 取出页大小
    auto id = reinterpret_cast<uintptr_t>(span->pageAddr) / ps; // 起始页号
    for (size_t i = 0; i < span->numPages; ++i) // 每一页
        pageMap_.clear(id + i);          // 写成未映射
}                                        // 函数结束

void PageCache::redirectPages(Span* from, Span* to) // 合并：from 的页改指向 to
{                                        // 函数开始
    const size_t ps = systemPageSize();  // 取出页大小
    auto id = reinterpret_cast<uintptr_t>(from->pageAddr) / ps; // from 的起始页号
    for (size_t i = 0; i < from->numPages; ++i) // from 的每一页
        pageMap_.set(id + i, to);        // 改成指向活下来的那段
}                                        // 函数结束

Span* PageCache::findSpanFromPageMap(void* ptr) const // 不加线程本地叶子缓存地查页表
{                                        // 函数开始
    return pageMap_.get(pageId(ptr));    // 页号 → span
}                                        // 函数结束

Span* PageCache::findLargeSpan(void* ptr) // 按起始地址查大对象表（要加锁）
{                                        // 函数开始
    if (!ptr)                            // 如果指针空
        return nullptr;                  // 找不到
    std::lock_guard<std::mutex> lock(mutex_); // 锁住页缓存（大对象表不是无锁的）
    auto it = largeSpans_.find(ptr);     // 必须正好是当初的 pageAddr
    if (it == largeSpans_.end())         // 如果没有
        return nullptr;                  // 找不到
    return it->second;                   // 返回这段 Span*
}                                        // 函数结束

namespace                                // 匿名命名空间（仅本文件可见）
{                                        // 匿名命名空间开始
// 下面到匿名命名空间结束：全是本 cpp 私有，PageCache.h 里没有

//头文件没有
thread_local void* t_mapLeaf = nullptr;  // 本线程上次用到的页表叶子
//头文件没有
thread_local uintptr_t t_mapLeafId = ~uintptr_t(0); // 那片叶子对应的页号前缀，初值故意无效
} // 匿名命名空间结束（以上全是头文件没有的）

Span* PageCache::findSpan(void* ptr)     // 无锁查小对象指针属于哪段 span
{                                        // 函数开始
    if (!ptr)                            // 如果指针空
        return nullptr;                  // 找不到
    return pageMap_.getCached(pageId(ptr), t_mapLeaf, t_mapLeafId); // 走页表，带叶子缓存
}                                        // 函数结束

size_t PageCache::findSizeClass(void* ptr) const // 先标签再页表，查出档位
{                                        // 函数开始
    if (!ptr)                            // 如果指针空
        return FREE_LIST_SIZE;           // 没有合法档
    const size_t ps = systemPageSize();  // 取出页大小
    if (const PageTag* tag = pageTagIfValid(ptr, ps)) // 如果页尾标签有效
        return tag->sizeClass;           // 直接返回标签里的档
    return pageMap_.getSizeClassCached(pageId(ptr), t_mapLeaf, t_mapLeafId); // 否则问页表
}                                        // 函数结束

size_t PageCache::reservedBytes()        // 还在系统手里占着的总字节
{                                        // 函数开始
    std::lock_guard<std::mutex> lock(mutex_); // 锁住，避免和申请 / 归还打架
    size_t total = 0;                    // 累加器
    for (const auto& allocation : systemAllocs_) // 每一次系统申请
        total += allocation.second;      // 加上这段字节数
    return total;                        // 返回总和
}                                        // 函数结束

size_t PageCache::cachedPageBytes()      // 空闲链表里缓存了多少字节
{                                        // 函数开始
    std::lock_guard<std::mutex> lock(mutex_); // 锁住
    return cachedFreePages_ * systemPageSize(); // 空闲页数乘页大小
}                                        // 函数结束

void PageCache::setCachedPageLimit(size_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cachedPageLimitBytes_ = bytes;
    reclaimCachedPagesLocked(bytes / systemPageSize());
}

size_t PageCache::cachedPageLimit()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cachedPageLimitBytes_;
}

size_t PageCache::releaseCachedPages()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return reclaimCachedPagesLocked(0);
}

size_t PageCache::reclaimCachedPagesLocked(size_t targetPages)
{
    const size_t ps = systemPageSize();
    size_t released = 0;
    for (auto it = freeSpans_.begin(); it != freeSpans_.end() && cachedFreePages_ > targetPages;)
    {
        Span** link = &it->second;
        while (*link && cachedFreePages_ > targetPages)
        {
            Span* span = *link;
            if (span->pageAddr != span->originAddr || span->numPages != span->originPages)
            {
                link = &span->next; // 部分空闲的 origin 仍被使用，不能归还
                continue;
            }
            *link = span->next;
            cachedFreePages_ -= span->numPages;
            released += span->numPages * ps;
            span->next = nullptr;
            span->inFreeList = false;
            releaseOrigin(span);
        }
        if (!it->second)
            it = freeSpans_.erase(it);
        else
            ++it;
    }
    return released;
}

void* PageCache::systemAlloc(size_t& numPages) // 向系统申请，页数可能被粒度抬高
{                                        // 函数开始
    const size_t ps = systemPageSize();  // 页大小
    const size_t gran = systemAllocGranularity(); // 系统分配粒度（Windows 上常大于一页）
    if (numPages > SIZE_MAX / ps)        // 如果页数乘页大小会溢出
        return nullptr;                  // 申请失败
    size_t bytes = numPages * ps;        // 先按页数算字节
    if (bytes > SIZE_MAX - (gran - 1))   // 如果再对齐会溢出
        return nullptr;                  // 申请失败
    bytes = (bytes + gran - 1) & ~(gran - 1); // 向上取整到粒度
    numPages = bytes / ps;               // 回写实际页数（可能比调用方要的多）
    void* ptr = allocPages(bytes);       // mmap / VirtualAlloc
    if (!ptr)                            // 如果系统给不出
        return nullptr;                  // 申请失败
    systemAllocs_.emplace_back(ptr, bytes); // 记下这段，析构或 releaseOrigin 要还
    return ptr;                          // 返回起始地址
}                                        // 函数结束

void PageCache::releaseOrigin(Span* span) // 把完整的一次系统申请还给系统
{                                        // 函数开始
    largeSpans_.erase(span->pageAddr);   // 若在大对象表里，先删掉
    if (findSpanFromPageMap(span->pageAddr) == span) // 如果页表还指着这段
        unregisterPages(span);           // 从页表清掉
    void* addr = span->originAddr;       // 当初系统给的起始地址
    size_t bytes = span->originPages * systemPageSize(); // 当初的字节数
    auto it = std::find_if(systemAllocs_.begin(), systemAllocs_.end(),
                           [addr](const std::pair<void*, size_t>& item) { // 在申请记录里找这段
                               return item.first == addr; // 地址对上就是这一笔
                           });
    if (it != systemAllocs_.end())       // 如果找到了
        systemAllocs_.erase(it);         // 从记录里去掉
    delete span;                         // 删掉 Span 元数据
    freePages(addr, bytes);              // 把虚存还给系统
}                                        // 函数结束

Span* PageCache::allocateSpan(size_t numPages) // 给中央要一段至少 numPages 的小对象 span
{                                        // 函数开始
    if (numPages == 0)                   // 如果要 0 页
        numPages = 1;                    // 至少 1 页
    std::lock_guard<std::mutex> lock(mutex_); // 锁住页缓存
    auto it = freeSpans_.lower_bound(numPages); // 找第一个页数 ≥ 需要的空闲桶
    if (it != freeSpans_.end())          // 如果有这么大或更大的空闲段
    {                                    // 命中空闲链分支开始
        Span* span = it->second;         // 取出该桶链表头
        cachedFreePages_ -= span->numPages; // 空闲页计数减去整段（后面拆开再加回余下的）
        if (span->next)                  // 如果桶里还有下一段
            freeSpans_[it->first] = span->next; // 头改成下一段
        else                             // 如果这是桶里最后一段
            freeSpans_.erase(it);        // 删掉这个桶
        span->next = nullptr;            // 摘下来
        span->inFreeList = false;        // 不再算空闲
        if (span->numPages > numPages)   // 如果这段比要的大
        {                                // 拆分分支开始
            Span* rest = new Span;       // 后半段新开一个 Span 对象
            rest->pageAddr = static_cast<char*>(span->pageAddr) + numPages * systemPageSize(); // 后半段起始
            rest->numPages = span->numPages - numPages; // 后半段页数
            rest->originAddr = span->originAddr; // 仍属同一次系统申请
            rest->originPages = span->originPages; // 原始页数不变
            rest->inFreeList = true;     // 后半段还是空闲
            rest->next = nullptr;        // 先断开
            auto& list = freeSpans_[rest->numPages]; // 按后半段页数找桶
            rest->next = list;           // 头插入该桶
            list = rest;                 // 新头
            cachedFreePages_ += rest->numPages; // 空闲页加回后半段
            registerPages(rest);         // 后半段各页指向 rest
            span->numPages = numPages;   // 前半段缩成调用方要的页数
            registerPages(span);         // 前半段各页指向 span
        }                                // 拆分分支结束
        else if (findSpanFromPageMap(span->pageAddr) != span) // 如果页表还没指向这段
        {                                // 补登记分支开始
            registerPages(span);         // 补上页表
        }                                // 补登记分支结束
        return span;                     // 把前半段（或整段）交给中央
    }                                    // 命中空闲链分支结束
    size_t allocPagesCount = numPages;   // 准备向系统要的页数
    void* memory = systemAlloc(allocPagesCount); // 向系统申请（页数可能被粒度抬高）
    if (!memory)                         // 如果系统给不出
        return nullptr;                  // 分配失败
    Span* span = new Span;               // 新建 Span 元数据
    span->pageAddr = memory;             // 起始就是系统给的地址
    span->numPages = allocPagesCount;    // 实际页数（可能多于请求）
    span->originAddr = memory;           // origin 就是自己（这次向系统要的起点）
    span->originPages = allocPagesCount; // 原始页数
    registerPages(span);                 // 各页写入页表
    return span;                         // 交给中央去切块
}                                        // 函数结束

Span* PageCache::allocateLargeSpan(size_t numPages) // 给大对象要一段整页
{                                        // 函数开始
    if (numPages == 0)                   // 如果要 0 页
        numPages = 1;                    // 至少 1 页
    std::lock_guard<std::mutex> lock(mutex_); // 锁住页缓存
    auto it = freeSpans_.lower_bound(numPages); // 同样先找 ≥ 需要页数的空闲段
    if (it != freeSpans_.end())          // 如果有
    {                                    // 命中空闲链分支开始
        Span* span = it->second;         // 取出桶头
        cachedFreePages_ -= span->numPages; // 空闲页减去整段
        if (span->next)                  // 如果桶里还有
            freeSpans_[it->first] = span->next; // 头后移
        else                             // 如果桶空了
            freeSpans_.erase(it);        // 删桶
        span->next = nullptr;            // 摘下来
        span->inFreeList = false;        // 不再算空闲
        if (span->numPages > numPages)   // 如果比要的大
        {                                // 拆分分支开始
            Span* rest = new Span;       // 后半段仍当小对象空闲页
            rest->pageAddr = static_cast<char*>(span->pageAddr) + numPages * systemPageSize(); // 后半段起始
            rest->numPages = span->numPages - numPages; // 后半段页数
            rest->originAddr = span->originAddr; // 仍属同一次系统申请
            rest->originPages = span->originPages; // 原始页数
            rest->inFreeList = true;     // 后半段回空闲链
            rest->next = nullptr;        // 先断开
            auto& list = freeSpans_[rest->numPages]; // 按页数进桶
            rest->next = list;           // 头插
            list = rest;                 // 新头
            cachedFreePages_ += rest->numPages; // 加回空闲
            registerPages(rest);         // 后半段进页表（仍可能给小对象用）
            span->numPages = numPages;   // 前半段缩成大对象要的页数
            if (findSpanFromPageMap(span->pageAddr) == span) // 如果页表还指着前半段
                unregisterPages(span);   // 大对象不走页表，注销
        }                                // 拆分分支结束
        else if (findSpanFromPageMap(span->pageAddr) == span) // 整段拿走且页表指着它
        {                                // 注销分支开始
            unregisterPages(span);       // 从页表拿掉，改走大对象表
        }                                // 注销分支结束
        span->isLarge = true;            // 标成大对象
        largeSpans_[span->pageAddr] = span; // 用起始地址登记
        return span;                     // 交给大对象路径，返回的是 pageAddr
    }                                    // 命中空闲链分支结束
    size_t allocPagesCount = numPages;   // 准备向系统要的页数
    void* memory = systemAlloc(allocPagesCount); // 向系统申请
    if (!memory)                         // 如果给不出
        return nullptr;                  // 分配失败
    Span* span = new Span;               // 新建 Span
    span->pageAddr = memory;             // 起始地址
    span->numPages = allocPagesCount;    // 实际页数
    span->originAddr = memory;           // origin 就是自己
    span->originPages = allocPagesCount; // 原始页数
    span->isLarge = true;                // 大对象
    largeSpans_[span->pageAddr] = span;  // 只进大对象表，不进页表
    return span;                         // 交出去
}                                        // 函数结束

void PageCache::deallocateSpan(Span* span) // 还一段 Span：合并、缓存或还给系统
{                                        // 函数开始
    if (!span)                           // 如果是空指针
        return;                          // 直接返回
    std::lock_guard<std::mutex> lock(mutex_); // 锁住页缓存
    const bool wasLarge = span->isLarge; // 先记下还之前是不是大对象（后面会清标志）
    if (wasLarge)                        // 如果是大对象
        largeSpans_.erase(span->pageAddr); // 从大对象表拿掉
    span->freeList = nullptr;            // 清空块链
    span->blockSize = 0;                 // 清空块大小
    span->blockCount = 0;                // 清空块数
    span->freeCount = 0;                 // 清空空闲块数
    span->sizeClass = 0;                 // 清空档位
    if (span->blocksPerPage > 0)         // 如果曾经按页写过标签
    {                                    // 清标签分支开始
        const size_t ps = systemPageSize(); // 页大小
        auto* base = static_cast<char*>(span->pageAddr); // 起始
        for (size_t p = 0; p < span->numPages; ++p) // 每一页
        {                                // 页循环开始
            auto* tag = reinterpret_cast<PageTag*>(base + (p + 1) * ps - kPageTagBytes); // 这一页页尾
            tag->magic = 0;              // 清 magic，避免旧档被误读
            tag->sizeClass = 0;          // 清档位
        }                                // 页循环结束
    }                                    // 清标签分支结束
    span->blocksPerPage = 0;             // 不再带标签
    span->isLarge = false;               // 还回来先当普通空闲页
    span->next = nullptr;                // 断开
    span->debugAllocMap.clear();         // 清调试位图
    span->debugLargeAllocated = false;   // 清大对象调试位
    if (!wasLarge)                       // 大对象不和旁边小对象空闲段合并
    {                                    // 合并分支开始
        void* nextAddr = static_cast<char*>(span->pageAddr) + span->numPages * systemPageSize(); // 紧邻后面那一页的地址
        Span* nextSpan = findSpanFromPageMap(nextAddr); // 看后面是不是一段已登记的 span
        if (nextSpan && nextSpan != span && nextSpan->inFreeList &&
            nextSpan->pageAddr == nextAddr &&
            nextSpan->originAddr == span->originAddr) // 后面空闲、正好接上、且同一 origin
        {                                // 向后合并分支开始
            if (removeFromFreeList(nextSpan)) // 先从空闲桶摘掉后面那段
            {                            // 摘成功分支开始
                span->numPages += nextSpan->numPages; // 页数加到当前这段
                redirectPages(nextSpan, span); // 后面那些页改指向当前这段
                delete nextSpan;         // 删掉后面那个 Span 对象
            }                            // 摘成功分支结束
        }                                // 向后合并分支结束
        if (span->pageAddr != span->originAddr) // 如果当前不是 origin 开头，前面才可能还有同一 origin 的段
        {                                // 向前合并分支开始
            void* prevPtr = static_cast<char*>(span->pageAddr) - 1; // 当前起始的前一个字节，落在前一段最后一页
            Span* prevCandidate = findSpanFromPageMap(prevPtr); // 查出前面那段
            if (prevCandidate && prevCandidate != span && prevCandidate->inFreeList &&
                prevCandidate->originAddr == span->originAddr) // 前面空闲且同一 origin
            {                            // 检查是否正好接上
                void* prevEnd = static_cast<char*>(prevCandidate->pageAddr) +
                                prevCandidate->numPages * systemPageSize(); // 前面那段的结尾
                if (prevEnd == span->pageAddr && removeFromFreeList(prevCandidate)) // 正好接上并且摘桶成功
                {                        // 合并进前面那段
                    redirectPages(span, prevCandidate); // 当前这些页改指向前面那段
                    prevCandidate->numPages += span->numPages; // 前面那段变长
                    delete span;         // 删掉当前这个 Span 对象
                    span = prevCandidate; // 后面缓存 / 还给系统都针对活下来的那段
                }                        // 合并进前面那段结束
            }                            // 检查结束
        }                                // 向前合并分支结束
    }                                    // 合并分支结束
    const size_t maxCachedPages = cachedPageLimitBytes_ / systemPageSize(); // 预算换成整页
    if (span->originAddr == span->pageAddr && span->numPages == span->originPages &&
        (cachedFreePages_ > maxCachedPages || span->numPages > maxCachedPages - cachedFreePages_)) // 完整 origin 超预算
    {                                    // 还给系统分支开始
        releaseOrigin(span);             // 整段还给操作系统
        return;                          // 结束
    }                                    // 还给系统分支结束
    span->inFreeList = true;             // 留下当空闲缓存
    auto& list = freeSpans_[span->numPages]; // 按现在的页数进桶
    span->next = list;                   // 头插
    list = span;                         // 新头
    cachedFreePages_ += span->numPages;  // 空闲页计数加上这段
}                                        // 函数结束

bool PageCache::removeFromFreeList(Span* span) // 从对应页数的空闲桶里摘掉这段
{                                        // 函数开始
    auto it = freeSpans_.find(span->numPages); // 按页数找桶
    if (it == freeSpans_.end())          // 如果没有这个桶
        return false;                    // 摘失败
    Span*& list = it->second;            // 桶的链表头
    if (list == span)                    // 如果正好是头
    {                                    // 摘头分支开始
        cachedFreePages_ -= span->numPages; // 空闲页减去
        list = span->next;               // 头后移
        if (!list)                       // 如果桶空了
            freeSpans_.erase(it);        // 删桶
        span->next = nullptr;            // 断开
        span->inFreeList = false;        // 不再算空闲
        return true;                     // 摘成功
    }                                    // 摘头分支结束
    Span* prev = list;                   // 从头发往后找
    while (prev && prev->next)           // 当后面还有节点
    {                                    // 循环开始
        if (prev->next == span)          // 如果下一个就是要摘的
        {                                // 找到分支开始
            cachedFreePages_ -= span->numPages; // 空闲页减去
            prev->next = span->next;     // 跳过它
            span->next = nullptr;        // 断开
            span->inFreeList = false;    // 不再算空闲
            return true;                 // 摘成功
        }                                // 找到分支结束
        prev = prev->next;               // 继续往后
    }                                    // 循环结束
    return false;                        // 链上没有这段
}                                        // 函数结束

} // Kama_memoryPool 命名空间结束
