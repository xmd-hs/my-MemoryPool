// 页缓存（PageCache）—— 三层架构的第三层，和操作系统打交道
//
// 职责：
//   - 按「页」（4KB）为单位管理大块内存（span）
//   - CentralCache 要多少页，从这里拿 span
//   - span 用完归还，相邻空闲 span 可以合并
//   - 向 OS 申请内存用 mmap / VirtualAlloc（见 Platform.h）
//
// 数据结构：
//   - freeSpans_：map<页数, span链表头>，按页数分组，拿 span 时找 >= 需要页数的
//   - spanMap_：map<起始地址, Span*>，用于回收时定位 span 并合并相邻
//   - systemAllocs_：记录所有向 OS 申请的原始块，析构时统一归还

#pragma once
#include "Common.h"
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace Kama_memoryPool
{

class PageCache
{
public:
    static const size_t PAGE_SIZE = 4096; // 4KB 页

    static PageCache& getInstance()
    {
        static PageCache instance;
        return instance;
    }

    // 分配 numPages 页的连续内存
    void* allocateSpan(size_t numPages);

    // 释放 span（会尝试合并相邻空闲 span）
    void deallocateSpan(void* ptr, size_t numPages);

private:
    PageCache() = default;
    ~PageCache();

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    // 一个 span = 连续若干页
    struct Span
    {
        void*  pageAddr;  // span 起始地址（页对齐）
        size_t numPages;  // span 占多少页
        Span*  next;      // 同页数链表的下一个 span
    };

    // 向 OS 申请（mmap / VirtualAlloc）
    void* systemAlloc(size_t numPages);
    // 从 freeSpans_ 的某条链表里摘掉一个 span
    bool removeFromFreeList(Span* span);

    // 按页数分组的空闲 span 链表：key=页数，value=该页数的 span 链表头
    std::map<size_t, Span*> freeSpans_;
    // 页起始地址 -> span，回收时用来找 span 并合并相邻
    std::map<void*, Span*> spanMap_;
    // 所有向 OS 申请的原始映射，析构时归还
    std::vector<std::pair<void*, size_t>> systemAllocs_;
    std::mutex mutex_;
};

} // namespace Kama_memoryPool
