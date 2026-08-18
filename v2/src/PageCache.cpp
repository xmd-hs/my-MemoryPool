#include "PageCache.h"
#include "Platform.h"
#include <iterator>

namespace Kama_memoryPool
{

PageCache::~PageCache()
{
    for (auto& kv : spanMap_)
    {
        delete kv.second;
    }
    spanMap_.clear();
    freeSpans_.clear();

    for (auto& alloc : systemAllocs_)
    {
        freePages(alloc.first, alloc.second);
    }
}

void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找合适的空闲span
    // lower_bound函数返回第一个大于等于numPages的元素的迭代器
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }
        span->next = nullptr;

        // 如果span大于需要的numPages则进行分割
        if (span->numPages > numPages) 
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + 
                                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 将超出部分放回空闲Span*列表头部，并记入spanMap_以便合并
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;
            spanMap_[newSpan->pageAddr] = newSpan;

            span->numPages = numPages;
        }

        // 记录span信息用于回收
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的span，向系统申请
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录span信息用于回收
    spanMap_[memory] = span;
    return memory;
}

void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找对应的span，没找到代表不是PageCache分配的内存，直接返回
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;

    Span* span = it->second;
    span->numPages = numPages;

    // 向后合并相邻空闲span
    void* nextAddr = static_cast<char*>(ptr) + span->numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    if (nextIt != spanMap_.end())
    {
        Span* nextSpan = nextIt->second;
        if (removeFromFreeList(nextSpan))
        {
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextIt);
            delete nextSpan;
        }
    }

    // 向前合并相邻空闲span（std::map 按地址有序）
    it = spanMap_.find(span->pageAddr);
    if (it != spanMap_.begin())
    {
        auto prevIt = std::prev(it);
        Span* prevSpan = prevIt->second;
        void* prevEnd = static_cast<char*>(prevSpan->pageAddr) +
                        prevSpan->numPages * PAGE_SIZE;
        if (prevEnd == span->pageAddr && removeFromFreeList(prevSpan))
        {
            prevSpan->numPages += span->numPages;
            spanMap_.erase(it);
            delete span;
            span = prevSpan;
        }
    }

    // 将合并后的span通过头插法插入空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

bool PageCache::removeFromFreeList(Span* span)
{
    auto it = freeSpans_.find(span->numPages);
    if (it == freeSpans_.end())
        return false;

    Span*& list = it->second;
    if (list == span)
    {
        list = span->next;
        if (!list)
            freeSpans_.erase(it);
        span->next = nullptr;
        return true;
    }

    Span* prev = list;
    while (prev && prev->next)
    {
        if (prev->next == span)
        {
            prev->next = span->next;
            span->next = nullptr;
            return true;
        }
        prev = prev->next;
    }
    return false;
}

void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;
    void* ptr = allocPages(size);
    if (!ptr)
        return nullptr;

    systemAllocs_.emplace_back(ptr, size);
    return ptr;
}

} // namespace Kama_memoryPool
