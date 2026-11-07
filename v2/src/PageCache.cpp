// PageCache 实现：页缓存，和操作系统打交道
//
// 核心数据结构：
//   - freeSpans_：map<页数, span链表头>，按页数分组管理空闲 span
//   - spanMap_：map<起始地址, Span*>，回收时定位 span 并合并相邻
//   - systemAllocs_：所有向 OS 申请的原始块，析构时统一归还
//
// 关键操作：
//   - allocateSpan：找 >= numPages 的空闲 span，大了就切割，没有就向 OS 要
//   - deallocateSpan：归还 span，向后+向前合并相邻空闲 span

#include "PageCache.h"
#include "Platform.h"
#include <iterator>

namespace Kama_memoryPool
{

// ===== 析构 =====

PageCache::~PageCache()
{
    // 释放所有 span 元数据
    for (auto& kv : spanMap_)
        delete kv.second;
    spanMap_.clear();
    freeSpans_.clear();

    // 把所有向 OS 申请的内存归还（VirtualFree / munmap）
    for (auto& alloc : systemAllocs_)
        freePages(alloc.first, alloc.second);
}

// ===== allocateSpan：分配 numPages 页 =====

void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 找第一个 >= numPages 的空闲 span
    // lower_bound 返回第一个 key >= numPages 的迭代器
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 从空闲链表中摘掉这个 span
        if (span->next)
            freeSpans_[it->first] = span->next;  // 链表还有下一个，换头
        else
            freeSpans_.erase(it);                 // 链表空了，删 key
        span->next = nullptr;

        // 如果 span 比需要的大，切割：前 numPages 页返回，剩下的放回空闲链表
        if (span->numPages > numPages)
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) +
                                numPages * PAGE_SIZE;  // 剩余部分的起始地址
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 剩余部分放回 freeSpans_ 对应页数的链表（头插）
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;
            spanMap_[newSpan->pageAddr] = newSpan;  // 记入 spanMap_ 以便回收合并

            span->numPages = numPages;
        }

        // 记录 span 信息，回收时用
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // —— 没有合适的空闲 span，向 OS 申请 ——
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建 span 元数据
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    spanMap_[memory] = span;
    return memory;
}

// ===== deallocateSpan：归还 span，尝试合并相邻 =====

void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 找到对应的 span（必须是 PageCache 分配的）
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;

    Span* span = it->second;
    span->numPages = numPages;

    // —— 向后合并：看紧挨着的后一个 span 是否空闲 ——
    void* nextAddr = static_cast<char*>(ptr) + span->numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    if (nextIt != spanMap_.end())
    {
        Span* nextSpan = nextIt->second;
        if (removeFromFreeList(nextSpan))  // 只有在空闲链表中才能合并
        {
            span->numPages += nextSpan->numPages;  // 吞并后面的 span
            spanMap_.erase(nextIt);
            delete nextSpan;
        }
    }

    // —— 向前合并：看紧挨着的前一个 span 是否空闲 ——
    // std::map 按 key（地址）有序，用 prev 找前一个
    it = spanMap_.find(span->pageAddr);
    if (it != spanMap_.begin())
    {
        auto prevIt = std::prev(it);
        Span* prevSpan = prevIt->second;
        void* prevEnd = static_cast<char*>(prevSpan->pageAddr) +
                        prevSpan->numPages * PAGE_SIZE;
        // 前一个 span 的结尾 == 当前 span 的开头，说明紧挨着
        if (prevEnd == span->pageAddr && removeFromFreeList(prevSpan))
        {
            prevSpan->numPages += span->numPages;  // 被前面的 span 吞并
            spanMap_.erase(it);
            delete span;
            span = prevSpan;  // 注意：现在 span 指向合并后的前一个 span
        }
    }

    // 合并后的 span 放回 freeSpans_ 对应页数的链表（头插）
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

// ===== 从空闲链表中摘掉一个 span =====

bool PageCache::removeFromFreeList(Span* span)
{
    auto it = freeSpans_.find(span->numPages);
    if (it == freeSpans_.end())
        return false;  // 这个页数根本没有链表

    Span*& list = it->second;
    if (list == span)
    {
        // span 是链表头
        list = span->next;
        if (!list)
            freeSpans_.erase(it);  // 链表空了，删 key
        span->next = nullptr;
        return true;
    }

    // span 在链表中间，找到它的前驱
    Span* prev = list;
    while (prev && prev->next)
    {
        if (prev->next == span)
        {
            prev->next = span->next;  // 跳过 span
            span->next = nullptr;
            return true;
        }
        prev = prev->next;
    }
    return false;  // 没找到（说明 span 不在空闲链表中，可能正在使用）
}

// ===== 向 OS 申请内存 =====

void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;
    void* ptr = allocPages(size);  // 见 Platform.h：mmap / VirtualAlloc
    if (!ptr)
        return nullptr;

    // 记录原始映射，析构时归还
    systemAllocs_.emplace_back(ptr, size);
    return ptr;
}

} // namespace Kama_memoryPool
