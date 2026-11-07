// CentralCache 的实现文件（第三版）
// 第二层，全局共享，按 size class × shard 分片降低锁竞争
// shards_[档 * 分片数 + 片] 挂 Span 链；块在 span->freeList 上
// fetchRange：本片 → TransferBin → 偷其他片 → 向页缓存切新 span
// returnRange：按 span 分组后还回各片；整段空了立刻还 PageCache

#include "CentralCache.h"                // 包含 CentralCache 头文件
#include "Platform.h"                    // 包含平台相关头文件

#include <algorithm>                     // 包含算法库（max）
#include <cstdint>                       // 包含定宽整数类型
#include <functional>                    // 包含 std::hash
#include <thread>                        // 包含线程 id
#include <vector>                        // 包含 std::vector

namespace Kama_memoryPool                // 进入 Kama_memoryPool 命名空间
{                                        // 命名空间开始

CentralCache::CentralCache()             // 构造函数：清空全部分片和箱子
{                                        // 函数开始
    for (auto& shard : shards_)          // 遍历每一个分片
    {                                    // 循环开始
        shard.lock.clear();              // 把这片的自旋锁清成未锁定
        shard.partial = nullptr;         // 这片还没有部分使用的 span
    }                                    // 循环结束
    for (auto& bin : transfer_)          // 遍历每一个 Transfer 箱子
    {                                    // 循环开始
        bin.head.store(nullptr, std::memory_order_relaxed); // 箱子链表头置空
        bin.count.store(0, std::memory_order_relaxed); // 箱子计数置 0
    }                                    // 循环结束
}                                        // 函数结束

// 当前线程的偏好分片（thread_local 缓存哈希结果，算一次就够）
size_t CentralCache::localShardId() const // 算出本线程固定用哪一片
{                                        // 函数开始
    static thread_local const size_t shard =
        std::hash<std::thread::id>{}(std::this_thread::get_id()) % kCentralShardCount;
    return shard;                        // 返回这个片号
}                                        // 函数结束

// 计算 (档位, 片号) 在 shards_ 数组里的下标
size_t CentralCache::shardIndex(size_t sizeClassIndex, size_t shardId) const // 算出分片数组下标
{                                        // 函数开始
    return sizeClassIndex * kCentralShardCount + shardId; // 下标 = 档位 * 8 + 片号
}                                        // 函数结束

// 向页缓存要一段页，切成这一档的块链表
Span* CentralCache::createSpan(size_t index) // 创建并切分一段新 span
{                                        // 函数开始
    const size_t blockSize = SizeClass::slotSize(index); // 取出这一档的槽大小
    const size_t usable = SizeClass::getSize(index); // 取出这一档用户能用的字节数
    const size_t ps = PageCache::pageSize(); // 取出系统页大小
    size_t targetBytes = 0;              // 打算向页缓存要多少字节，先置 0
    if (usable <= 256)                   // 如果块不大于 256 字节
        targetBytes = 32 * 1024;         // 目标 span 做成 32KiB
    else if (usable <= 1024)             // 否则如果块不大于 1024 字节
        targetBytes = 128 * 1024;        // 目标 span 做成 128KiB
    else if (usable <= 4096)             // 否则如果块不大于 4096 字节
        targetBytes = 512 * 1024;        // 目标 span 做成 512KiB
    else                                 // 更大的档
        targetBytes = 2 * 1024 * 1024;   // 目标 span 做成 2MiB
    targetBytes = std::max(ps, targetBytes); // 至少要有一页
    if (blockSize > targetBytes)         // 如果单块已经比目标还大
        targetBytes = ((blockSize + ps - 1) / ps) * ps; // 至少凑够放下一块的整页
    size_t numPages = targetBytes / ps;  // 换成页数
    if (numPages == 0)                   // 如果算出来是 0 页
        numPages = 1;                    // 至少要 1 页
    Span* span = PageCache::getInstance().allocateSpan(numPages); // 向页缓存要这么多页
    if (!span)                           // 如果页缓存给不出
        return nullptr;                  // 创建失败
    const size_t bytes = span->numPages * ps; // 这段 span 实际有多少字节
    void* head = nullptr;                // 切出来的块链表头，先空
    size_t n = 0;                        // 实际切出多少块，先 0
    size_t blocksPerPage = 0;            // 每页多少块（有页标签时才用）
    char* start = static_cast<char*>(span->pageAddr); // 把起始地址转成字节指针
    if (blockSize != 0 && blockSize <= 256 && blockSize <= ps - kPageTagBytes) // 如果是小块且页尾还能放下标签
    {                                    // 带页尾标签的切块分支开始
        blocksPerPage = (ps - kPageTagBytes) / blockSize; // 每页扣掉标签后能切几块
        char* prev = nullptr;            // 上一块的地址，用来串链表
        for (size_t p = 0; p < span->numPages; ++p) // 从第 0 页走到最后一页
        {                                // 页循环开始
            char* page = start + p * ps; // 算出这一页的起始地址
            const uintptr_t pageBase = reinterpret_cast<uintptr_t>(page); // 把页起始转成整数，算 magic
            auto* tag = reinterpret_cast<PageTag*>(page + ps - kPageTagBytes); // 指向这一页末尾的标签
            tag->magic = pageTagCookie(pageBase); // 写入 magic
            tag->sizeClass = static_cast<uint32_t>(index); // 写入这一档的档位
            tag->_pad = 0;               // 填充字段清零
            for (size_t i = 0; i < blocksPerPage; ++i) // 把这一页里的每一块串起来
            {                            // 块循环开始
                char* blk = page + i * blockSize; // 算出第 i 块的地址
                if (prev)                // 如果已经有上一块
                    *reinterpret_cast<void**>(prev) = blk; // 让上一块的 next 指向这块
                else                     // 否则这是第一块
                    head = blk;          // 链表头就是这块
                prev = blk;              // 记下这块，下一轮当上一块
                ++n;                     // 块数加 1
            }                            // 块循环结束
        }                                // 页循环结束
        if (prev)                        // 如果至少切出了一块
            *reinterpret_cast<void**>(prev) = nullptr; // 最后一块的 next 置空
    }                                    // 带页尾标签的切块分支结束
    else                                 // 更大的块：整段连续切，不写页标签
    {                                    // 无标签切块分支开始
        n = blockSize ? bytes / blockSize : 0; // 整段能切多少整块
        if (n == 0)                      // 如果一块都切不出来
        {                                // 失败分支开始
            PageCache::getInstance().deallocateSpan(span); // 把这段 span 还给页缓存
            return nullptr;              // 创建失败
        }                                // 失败分支结束
        for (size_t i = 0; i + 1 < n; ++i) // 从第 0 块连到倒数第二块
            *reinterpret_cast<void**>(start + i * blockSize) = start + (i + 1) * blockSize; // 第 i 块的 next 指向下一块
        *reinterpret_cast<void**>(start + (n - 1) * blockSize) = nullptr; // 最后一块的 next 置空
        head = start;                    // 链表头是第一块
    }                                    // 无标签切块分支结束
    if (n == 0)                          // 如果最终一块都没有
    {                                    // 失败分支开始
        PageCache::getInstance().deallocateSpan(span); // 把这段 span 还给页缓存
        return nullptr;                  // 创建失败
    }                                    // 失败分支结束
    span->blockSize = blockSize;         // 记录每块多大
    span->blockCount = n;                // 记录一共多少块
    span->freeCount = n;                 // 刚切完，全部空闲
    span->freeList = head;               // 空闲链就是刚串好的链表
    span->blocksPerPage = blocksPerPage; // 记录每页块数（无标签则为 0）
    span->sizeClass = index;             // 记录属于哪一档
    span->isLarge = false;               // 这是小对象 span，不是大对象
    span->next = nullptr;                // 还没挂到分片的 partial 链上
    span->centralShard.store(static_cast<size_t>(-1), std::memory_order_relaxed); // 归属分片先标未发布，调用方持锁后再写
    span->debugLargeAllocated = false;   // 调试：这不是已分配的大对象
    if (kDebugGuardsEnabled)   // 如果打开了调试护栏
        span->debugAllocMap.assign(n, 0); // 给每一块建一个「未分配」位
    PageCache::getInstance().refreshPageTags(span); // 把档位刷进页表，给不带大小的释放用
    return span;                         // 把切好的 span 交给调用方
}                                        // 函数结束

// 从分片的 partial 链表里摘掉指定 span
void CentralCache::unlinkPartial(size_t shardIdx, Span* span) // 从该片 partial 链上摘掉这段 span
{                                        // 函数开始
    Span** cursor = &shards_[shardIdx].partial; // cursor 指向链表头指针本身，方便原地摘节点
    while (*cursor)                      // 当当前节点不为空
    {                                    // 循环开始
        if (*cursor == span)             // 如果找到了要摘的这段
        {                                // 找到分支开始
            *cursor = span->next;        // 把这段从链上跳过
            span->next = nullptr;        // 清空这段的 next
            return;                      // 摘完返回
        }                                // 找到分支结束
        cursor = &(*cursor)->next;       // 走到下一个节点的 next 字段
    }                                    // 循环结束
}                                        // 函数结束

// 已持该片锁：从 partial 头 span 弹出最多 limit 个块
size_t CentralCache::popRangeFromShardLocked(size_t shardIdx, size_t limit,
                                             void*& head, void*& tail) // 从分片弹出一段块
{                                        // 函数开始
    head = nullptr;                      // 先把返回的链表头置空
    tail = nullptr;                      // 先把返回的链表尾置空
    if (limit == 0)                      // 如果一个都不想要
        return 0;                        // 弹出 0 个
    Span* span = shards_[shardIdx].partial; // 取出这片 partial 链的头 span
    if (!span || !span->freeList)        // 如果没有 span，或这段已经没有空闲块
        return 0;                        // 弹出 0 个
    head = span->freeList;               // 弹出段的头就是 span 当前空闲链头
    tail = head;                         // 尾先等于头，再往下走
    size_t count = 1;                    // 已经算上第一块
    while (count < limit && *reinterpret_cast<void**>(tail)) // 还没够数，且后面还有块
    {                                    // 循环开始
        tail = *reinterpret_cast<void**>(tail); // 尾巴走到下一块
        ++count;                         // 弹出个数加 1
    }                                    // 循环结束
    span->freeList = *reinterpret_cast<void**>(tail); // span 里剩下的从尾巴的 next 开始
    *reinterpret_cast<void**>(tail) = nullptr; // 掐断弹出段，尾巴后面不再连 span
    span->freeCount -= count;            // span 的空闲块数减去弹出的个数
    if (span->freeCount == 0)            // 如果这段 span 已经没有空闲块
    {                                    // 摘掉空 span 分支开始
        shards_[shardIdx].partial = span->next; // 把头 span 从 partial 上摘掉
        span->next = nullptr;            // 清空这段的 next
    }                                    // 摘掉空 span 分支结束
    return count;                        // 返回实际弹出了多少块
}                                        // 函数结束

// 已持分片锁：把一串块接到 span 的空闲链上
// None=还成功，FullSpan=整段空了可还页缓存，Retry=归属片对不上
CentralCache::ReturnAction CentralCache::returnSpanGroupLocked(
    Span* span, void* head, size_t shardIdx, size_t sizeClassIndex) // 把一组块还回指定 span
{                                        // 函数开始
    if (!span || !head || span->sizeClass != sizeClassIndex) // 如果 span 空、链空，或档位对不上
        return ReturnAction::None;       // 当作没做事
    if (span->centralShard.load(std::memory_order_relaxed) != shardIdx) // 如果这段现在不属于这片
        return ReturnAction::Retry;      // 让调用方换锁重试
    size_t count = 0;                    // 这串有多少块，先 0
    for (void* cur = head; cur; cur = *reinterpret_cast<void**>(cur)) // 顺着这串往下走
        ++count;                         // 每看到一块就加 1
    const bool wasEmpty = (span->freeCount == 0); // 还之前这段是不是已经没有空闲块
    void* tail = head;                   // 从这串的头开始找尾巴
    while (*reinterpret_cast<void**>(tail)) // 当尾巴后面还有块
        tail = *reinterpret_cast<void**>(tail); // 尾巴往下走
    *reinterpret_cast<void**>(tail) = span->freeList; // 把原来的空闲链接到这串后面
    span->freeList = head;               // 空闲链头改成这串的头
    span->freeCount += count;            // 空闲块数加上还回来的个数
    if (span->freeCount == span->blockCount) // 如果全部块都空闲了
    {                                    // 整段空了分支开始
        unlinkPartial(shardIdx, span);   // 先从 partial 上摘掉
        return ReturnAction::FullSpan;   // 告诉调用方可以还页缓存
    }                                    // 整段空了分支结束
    if (wasEmpty)                        // 如果还之前这段不在 partial 上
    {                                    // 重新挂回 partial 分支开始
        span->next = shards_[shardIdx].partial; // 把头插到这片 partial 前面
        shards_[shardIdx].partial = span; // 这段成为新的 partial 头
    }                                    // 重新挂回 partial 分支结束
    return ReturnAction::None;           // 还成功，span 还没满
}                                        // 函数结束

// 把多组块还回各 span 所在的分片
void CentralCache::returnGroups(size_t index, size_t preferredShardId,
                                const std::vector<SpanFreeGroup>& groups) // 按组还回各 span
{                                        // 函数开始
    if (groups.empty())                  // 如果一组都没有
        return;                          // 直接返回
    std::vector<Span*> fullSpans;        // 收集整段已空、要还页缓存的 span（用栈上 vector，不用 TLS）
    fullSpans.reserve(groups.size());    // 按组数预留空间
    const size_t preferredShard = shardIndex(index, preferredShardId); // 算出偏好分片在数组里的下标
    for (const auto& entry : groups)     // 逐组处理
    {                                    // 组循环开始
        Span* span = entry.span;         // 取出这组所属的 span
        void* head = entry.head;         // 取出这组要还的块链表头
        if (!span || !head)              // 如果 span 或链是空的
            continue;                    // 跳过这一组
        for (;;)                         // 归属片可能变，失败就换锁重试
        {                                // 重试循环开始
            size_t home = span->centralShard.load(std::memory_order_acquire); // 读出这段当前归属哪一片
            if (home == static_cast<size_t>(-1)) // 如果还没发布归属
                home = preferredShard;   // 先用调用方的偏好片
            ReturnAction action = ReturnAction::None; // 这次归还的结果，先当成功
            {                            // 持锁区间开始
                SpinLock guard(shards_[home].lock); // 锁住算出来的那一片
                size_t current = span->centralShard.load(std::memory_order_relaxed); // 锁内再读一次归属
                if (current == static_cast<size_t>(-1)) // 如果锁内发现仍未发布
                {                        // 首次发布分支开始
                    span->centralShard.store(home, std::memory_order_relaxed); // 把归属写成当前这片
                    current = home;      // 本地也记成这片
                }                        // 首次发布分支结束
                if (current != home)     // 如果锁内发现归属已经不是这片
                    continue;            // 放开锁，下一轮换对的片
                action = returnSpanGroupLocked(span, head, home, index); // 在锁内把这串块还回去
            }                            // 持锁区间结束
            if (action == ReturnAction::Retry) // 如果锁内判定片不对
                continue;                // 再读归属，重试
            if (action == ReturnAction::FullSpan) // 如果这段已经全空
                fullSpans.push_back(span); // 先记下，锁外再还页缓存
            break;                       // 这一组处理完，跳出重试
        }                                // 重试循环结束
    }                                    // 组循环结束
    for (Span* span : fullSpans)         // 遍历所有整段已空的 span
        PageCache::getInstance().deallocateSpan(span); // 把整段还给页缓存
}                                        // 函数结束

// 把一条块链表按所属 span 分组
void CentralCache::collectGroups(void* list, size_t index, std::vector<SpanFreeGroup>& groups) // 按 span 拆组
{                                        // 函数开始
    groups.clear();                      // 先清空输出数组
    PageCache& cache = PageCache::getInstance(); // 拿到页缓存，用来查块属于哪段 span
    Span* lastSpan = nullptr;            // 上一块所属的 span，用来少查页表
    size_t lastIdx = static_cast<size_t>(-1); // 上一块落在 groups 里的下标
    uintptr_t lastPage = ~uintptr_t(0);  // 上一块所在的页号，先标成无效
    const size_t shift = systemPageShift(); // 地址右移几位得到页号
    void* cur = list;                    // 从链表头开始走
    while (cur)                          // 当还有块
    {                                    // 循环开始
        void* next = *reinterpret_cast<void**>(cur); // 先读出下一块，后面会改这块的 next
        const uintptr_t page = reinterpret_cast<uintptr_t>(cur) >> shift; // 算出这块所在页号
        Span* span = (page == lastPage) ? lastSpan : cache.findSpan(cur); // 同页就复用上一次的 span，否则查页表
        lastPage = page;                 // 记下这一页，下一块可能还能复用
        if (span && span->sizeClass == index) // 如果找到了 span，且档位对得上
        {                                // 合法 span 分支开始
            size_t idx = static_cast<size_t>(-1); // 这组在 groups 里的下标，先无效
            if (span == lastSpan)        // 如果还是上一段 span
            {                            // 同 span 分支开始
                idx = lastIdx;           // 直接用缓存的下标
            }                            // 同 span 分支结束
            else                         // 换了一段 span
            {                            // 换 span 分支开始
                for (size_t i = 0; i < groups.size(); ++i) // 在已有组里找这段 span
                {                        // 查找循环开始
                    if (groups[i].span == span) // 如果第 i 组就是这段
                    {                    // 找到分支开始
                        idx = i;         // 记下下标
                        break;           // 停止查找
                    }                    // 找到分支结束
                }                        // 查找循环结束
                if (idx == static_cast<size_t>(-1)) // 如果还没有这一组
                {                        // 新建组分支开始
                    groups.push_back(SpanFreeGroup{span, nullptr}); // 新加一组，链头先空
                    idx = groups.size() - 1; // 下标就是最后一组
                }                        // 新建组分支结束
                lastSpan = span;         // 缓存这段 span
                lastIdx = idx;           // 缓存这组下标
            }                            // 换 span 分支结束
            *reinterpret_cast<void**>(cur) = groups[idx].head; // 把头插入这一组
            groups[idx].head = cur;      // 这块成为这一组的新头
        }                                // 合法 span 分支结束
        else                             // 查不到，或档位不对
        {                                // 丢弃这块分支开始
            lastSpan = span;             // 还是记下 span，避免下一块误复用下标
            lastIdx = static_cast<size_t>(-1); // 下标作废
        }                                // 丢弃这块分支结束
        cur = next;                      // 走到下一块
    }                                    // 循环结束
}                                        // 函数结束

// 这一档 TransferBin 最多囤多少块：小块多囤，大块少囤
size_t CentralCache::transferCap(size_t index) const // 计算 Transfer 箱子容量
{                                        // 函数开始
    const size_t bytes = SizeClass::getSize(index); // 取出这一档的块大小
    if (bytes <= 256)                    // 如果块不大于 256 字节
        return 256;                      // 最多囤 256 块
    if (bytes <= 4096)                   // 如果块不大于 4096 字节
        return 64;                       // 最多囤 64 块
    return 16;                           // 更大的档最多囤 16 块
}                                        // 函数结束

// 从 TransferBin 弹出最多 limit 个块
size_t CentralCache::popTransfer(size_t index, size_t limit, void*& head, void*& tail) // 从 Transfer 箱子拿块
{                                        // 函数开始
    head = nullptr;                      // 返回链表头先空
    tail = nullptr;                      // 返回链表尾先空
    if (limit == 0)                      // 如果一个都不想要
        return 0;                        // 拿到 0 个
    auto& bin = transfer_[index];        // 取出这一档的 Transfer 箱子
    if (!bin.head.load(std::memory_order_relaxed)) // 如果箱子是空的
        return 0;                        // 拿到 0 个
    void* list = bin.head.exchange(nullptr, std::memory_order_acq_rel); // 原子摘走整箱
    if (!list)                           // 如果摘到的是空（别人刚拿走）
        return 0;                        // 拿到 0 个
    head = list;                         // 弹出段的头就是整箱原来的头
    tail = list;                         // 尾巴先等于头
    size_t got = 1;                      // 已经算上第一块
    while (got < limit)                  // 还没拿到够的个数
    {                                    // 循环开始
        void* next = *reinterpret_cast<void**>(tail); // 看尾巴后面还有没有
        if (!next)                       // 如果已经到链尾
            break;                       // 停止往下走
        tail = next;                     // 尾巴走到下一块
        ++got;                           // 拿到的个数加 1
    }                                    // 循环结束
    void* rest = *reinterpret_cast<void**>(tail); // 多出来的后半段
    *reinterpret_cast<void**>(tail) = nullptr; // 掐断弹出段
    if (rest)                            // 如果还有剩的
    {                                    // 放回箱子分支开始
        void* restTail = rest;           // 从剩余段的头找尾巴
        while (void* n = *reinterpret_cast<void**>(restTail)) // 当后面还有块
            restTail = n;                // 尾巴往下走
        void* old = bin.head.load(std::memory_order_relaxed); // 读出当前箱头
        do                               // 准备 CAS 把头插回去
        {                                // CAS 循环开始
            *reinterpret_cast<void**>(restTail) = old; // 剩余段尾巴指向当前箱头
        } while (!bin.head.compare_exchange_weak(
            old, rest, std::memory_order_release, std::memory_order_relaxed)); // 失败则 old 已更新，重试
    }                                    // 放回箱子分支结束
    size_t oldCount = bin.count.load(std::memory_order_relaxed); // 读出近似计数
    while (true)                         // 用 CAS 减去本次拿走的个数
    {                                    // CAS 循环开始
        const size_t next = oldCount >= got ? oldCount - got : 0; // 不够减就变成 0
        if (bin.count.compare_exchange_weak(
                oldCount, next, std::memory_order_relaxed, std::memory_order_relaxed)) // 减成功就结束
            break;                       // 跳出循环
    }                                    // CAS 循环结束
    return got;                          // 返回实际拿到的块数
}                                        // 函数结束

// 把一批块推进 TransferBin；箱子装不下的走 returnRange
void CentralCache::pushTransfer(void* head, size_t count, size_t index) // 把一块链表推进 Transfer
{                                        // 函数开始
    if (!head || count == 0 || index >= FREE_LIST_SIZE) // 如果链空、个数为 0，或档位越界
        return;                          // 直接返回
    const size_t cap = transferCap(index); // 取出这一档箱子的容量
    auto& bin = transfer_[index];        // 取出这一档的箱子
    const size_t used = bin.count.load(std::memory_order_relaxed); // 读出箱子大概用了多少
    const size_t room = used < cap ? cap - used : 0; // 还能再收多少
    if (room == 0)                       // 如果箱子已经满了
    {                                    // 满箱分支开始
        returnRange(head, count, index); // 整串按 span 还回各分片
        return;                          // 结束
    }                                    // 满箱分支结束
    void* transferHead = head;           // 准备推进箱子的那一段，默认整串
    size_t transferCount = count;        // 推进箱子的个数，默认全部
    void* overflowHead = nullptr;        // 多出来要 returnRange 的头，先空
    size_t overflowCount = 0;            // 多出来的个数，先 0
    if (count > room)                    // 如果整串比剩余容量还长
    {                                    // 需要拆开分支开始
        void* split = head;              // 从链表头走到「能进箱子的最后一块」
        for (size_t i = 1; i < room; ++i) // 向前走 room-1 步
            split = *reinterpret_cast<void**>(split); // 走到下一块
        overflowHead = *reinterpret_cast<void**>(split); // 拆分点后面就是溢出段
        *reinterpret_cast<void**>(split) = nullptr; // 掐断，前半段只留 room 个
        transferCount = room;            // 进箱子的个数就是剩余容量
        overflowCount = count - room;    // 溢出个数是多出来的那些
    }                                    // 需要拆开分支结束
    void* tail = transferHead;           // 从进箱这段的头找尾巴
    while (void* next = *reinterpret_cast<void**>(tail)) // 当后面还有块
        tail = next;                     // 尾巴往下走
    void* old = bin.head.load(std::memory_order_relaxed); // 读出当前箱头
    do                                   // 准备 CAS 头插
    {                                    // CAS 循环开始
        *reinterpret_cast<void**>(tail) = old; // 进箱段尾巴指向当前箱头
    } while (!bin.head.compare_exchange_weak(
        old, transferHead, std::memory_order_release, std::memory_order_relaxed)); // 失败则 old 已更新，重试
    bin.count.fetch_add(transferCount, std::memory_order_relaxed); // 近似计数加上进箱的个数
    if (overflowHead)                    // 如果有溢出段
        returnRange(overflowHead, overflowCount, index); // 溢出段按 span 还回各分片
}                                        // 函数结束

void CentralCache::drainTransfers()
{
    for (size_t index = 0; index < FREE_LIST_SIZE; ++index)
    {
        void* head = nullptr;
        void* tail = nullptr;
        const size_t count = popTransfer(index, static_cast<size_t>(-1), head, tail);
        if (count)
            returnRange(head, count, index);
    }
}

// 批量取块：本片 → Transfer → 偷其他片 → 切新 span
void* CentralCache::fetchRange(size_t index, size_t batchNum, size_t& fetchedCount) // 向这一档批发一批块
{                                        // 函数开始
    fetchedCount = 0;                    // 实际拿到的个数先写成 0
    if (index >= FREE_LIST_SIZE || batchNum == 0) // 如果档位越界，或一个都不想要
        return nullptr;                  // 取失败
    const size_t shardId = localShardId(); // 本线程偏好的片号
    const size_t primaryShard = shardIndex(index, shardId); // 偏好片在数组里的下标
    void* head = nullptr;                // 最终交给调用方的链表头
    void* tail = nullptr;                // 最终链表的尾巴，用来拼接后续段
    size_t got = 0;                      // 已经拿到多少块
    while (got < batchNum)               // 还没凑够要的个数
    {                                    // 循环开始
        void* rangeHead = nullptr;       // 这一轮摘到的一段的头
        void* rangeTail = nullptr;       // 这一轮摘到的一段的尾
        size_t rangeCount = 0;           // 这一轮摘到多少块
        {                                // 本片持锁区间开始
            SpinLock guard(shards_[primaryShard].lock); // 锁住偏好片
            rangeCount = popRangeFromShardLocked(primaryShard, batchNum - got,
                                                 rangeHead, rangeTail); // 从本片头 span 摘还缺的那些
        }                                // 本片持锁区间结束
        if (!rangeCount)                 // 如果本片没有
            rangeCount = popTransfer(index, batchNum - got, rangeHead, rangeTail); // 从 Transfer 箱子拿
        if (!rangeCount)                 // 如果箱子也没有
        {                                // 偷其他片分支开始
            for (size_t i = 1; i < kCentralShardCount && !rangeCount; ++i) // 从下一片开始转一圈
            {                            // 偷片循环开始
                const size_t otherShard = shardIndex(index, (shardId + i) % kCentralShardCount); // 算出另一片的下标
                SpinLock guard(shards_[otherShard].lock); // 锁住那一片
                rangeCount = popRangeFromShardLocked(otherShard, batchNum - got,
                                                     rangeHead, rangeTail); // 从那片头 span 偷块
            }                            // 偷片循环结束
        }                                // 偷其他片分支结束
        if (rangeCount)                  // 如果这一轮摘到了块
        {                                // 拼接到结果链分支开始
            if (!head)                   // 如果结果链还是空的
                head = rangeHead;        // 结果头就是这一段
            else                         // 如果结果链已有前半段
                *reinterpret_cast<void**>(tail) = rangeHead; // 接到当前尾巴后面
            tail = rangeTail;            // 更新结果链尾巴
            got += rangeCount;           // 累加已拿到的个数
            continue;                    // 不够就再摘一轮
        }                                // 拼接到结果链分支结束
        Span* span = createSpan(index);  // 前面都没有，向页缓存切一段新的
        if (!span)                       // 如果页缓存也给不出
            break;                       // 能拿多少算多少，结束循环
        {                                // 把新 span 挂到本片并摘块，持锁区间开始
            SpinLock guard(shards_[primaryShard].lock); // 锁住偏好片
            span->centralShard.store(primaryShard, std::memory_order_relaxed); // 这段归属本片
            span->next = shards_[primaryShard].partial; // 头插到本片 partial
            shards_[primaryShard].partial = span; // 新 span 成为本片 partial 头
            rangeHead = nullptr;         // 清空这一轮的头
            rangeTail = nullptr;         // 清空这一轮的尾
            rangeCount = popRangeFromShardLocked(primaryShard, batchNum - got,
                                                 rangeHead, rangeTail); // 从刚挂上的 span 摘还缺的块
        }                                // 持锁区间结束
        if (rangeCount)                  // 如果从新 span 摘到了
        {                                // 拼接分支开始
            if (!head)                   // 如果结果链还空
                head = rangeHead;        // 结果头就是这一段
            else                         // 如果已有前半段
                *reinterpret_cast<void**>(tail) = rangeHead; // 接到尾巴后面
            tail = rangeTail;            // 更新尾巴
            got += rangeCount;           // 累加个数
        }                                // 拼接分支结束
    }                                    // 循环结束
    fetchedCount = got;                  // 把实际个数写给调用方
    return head;                         // 返回这块链表的头（可能不足 batchNum）
}                                        // 函数结束

// 归还一批块：先按 span 分组，再还回各分片
void CentralCache::returnRange(void* start, size_t blockCount, size_t index) // 归还一批块
{                                        // 函数开始
    if (!start || index >= FREE_LIST_SIZE) // 如果链空或档位越界
        return;                          // 直接返回
    (void)blockCount;                    // 个数参数现网不用，分组时自己数
    std::vector<SpanFreeGroup> groups;   // 用来按 span 收组
    groups.reserve(8);                   // 通常只涉及少数几段 span
    collectGroups(start, index, groups); // 把链表拆成按 span 分组
    returnGroups(index, localShardId(), groups); // 还回各 span 所在分片
}                                        // 函数结束

} // Kama_memoryPool 命名空间结束
