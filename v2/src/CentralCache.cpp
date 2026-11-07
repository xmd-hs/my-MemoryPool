// CentralCache 实现：中央缓存，线程间共享，按 size class 分片
//
// 核心机制：
//   1. 每个 size class 一把自旋锁，临界区只做链表摘/挂，极短
//   2. 从 PageCache 拿的 span 切成小块，用侵入式链表串起来
//   3. 每个 span 配一个 SpanTracker，记录「总块数 / 空闲块数」
//   4. 延迟归还：不是每次归还都检查 span 是否全空，而是累积 N 次或超时才扫描
//
// 注意：getSpanTracker 是线性扫描 spanTrackers_，v2 的性能瓶颈
//       v3 用页表（PageTag）把 O(n) 降到 O(1)

#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace Kama_memoryPool
{

const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};  // 1 秒

// 小对象固定拿 8 页（32KB），一次切很多块，减少向 PageCache 要的次数
static const size_t SPAN_PAGES = 8;

// ===== 构造 =====

CentralCache::CentralCache()
{
    // 所有档位的链表、锁、延迟计数初始化为空/零
    for (auto& ptr : centralFreeList_)
        ptr.store(nullptr, std::memory_order_relaxed);
    for (auto& lock : locks_)
        lock.clear();
    for (auto& count : delayCounts_)
        count.store(0, std::memory_order_relaxed);
    for (auto& time : lastReturnTimes_)
        time = std::chrono::steady_clock::now();
}

// ===== fetchRange：给 ThreadCache 一块内存 =====

void* CentralCache::fetchRange(size_t index)
{
    if (index >= FREE_LIST_SIZE)
        return nullptr;

    // —— 加自旋锁 ——
    // 临界区短（摘链表指针），自旋比 mutex 进内核快
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    void* result = nullptr;
    try
    {
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // —— 中央也空了：从 PageCache 拿 span，切成小块 ——
            size_t size = SizeClass::getSize(index);
            result = fetchFromPageCache(size);
            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            char* start = static_cast<char*>(result);

            // 算页数：小对象固定 8 页，大对象按需向上取整到页
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
                             SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;  // 这个 span 能切多少块

            if (blockNum > 1)
            {
                // 把 span 切成 blockNum 个 size 字节的块，用侵入式链表串起来
                for (size_t i = 1; i < blockNum; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;  // 前一块指向后一块
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;  // 最后一块为空

                // 第一个块返回给调用者，剩余的挂到中央链表
                void* next = *reinterpret_cast<void**>(result);
                *reinterpret_cast<void**>(result) = nullptr;
                centralFreeList_[index].store(next, std::memory_order_release);

                // —— 记录 SpanTracker ——
                // 作用：追踪这个 span 的空闲块数，全空时归还给 PageCache
                // 为什么需要：CentralCache 管理的小块可能不连续（来自不同 span），
                //              但 PageCache 要求归还连续的 span，所以要按 span 分组
                auto tracker = std::make_unique<SpanTracker>();
                tracker->spanAddr.store(start, std::memory_order_release);
                tracker->numPages.store(numPages, std::memory_order_release);
                tracker->blockCount.store(blockNum, std::memory_order_release);
                tracker->freeCount.store(blockNum - 1, std::memory_order_release);  // 第一个已被拿走
                {
                    std::lock_guard<std::mutex> lock(trackerMutex_);
                    spanTrackers_.push_back(std::move(tracker));
                }
            }
        }
        else
        {
            // —— 中央有块：摘链表头 ——
            void* next = *reinterpret_cast<void**>(result);
            *reinterpret_cast<void**>(result) = nullptr;
            centralFreeList_[index].store(next, std::memory_order_release);

            // 对应 span 的空闲块数 -1
            SpanTracker* tracker = getSpanTracker(result);
            if (tracker)
            {
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
            }
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
    return result;
}

// ===== returnRange：接收 ThreadCache 归还的一批块 =====

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    size_t blockSize = SizeClass::getSize(index);
    size_t blockCount = size / blockSize;  // 归还了多少块

    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        // —— 找到归还链表的尾巴 ——
        void* end = start;
        size_t count = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && count < blockCount) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }

        // 头插法：把归还链表接到中央链表前面
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = current;
        centralFreeList_[index].store(start, std::memory_order_release);

        // —— 延迟归还检查 ——
        // 不是每次都扫描（遍历链表 O(n) 太贵），而是累积到一定次数或超时才扫
        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();

        if (shouldPerformDelayedReturn(index, currentCount, currentTime))
        {
            performDelayedReturn(index);
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

// ===== 延迟归还：是否该扫描 =====

bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount,
    std::chrono::steady_clock::time_point currentTime)
{
    // 触发条件：累积归还次数 >= 48，或距上次检查超过 1 秒
    if (currentCount >= MAX_DELAY_COUNT)
        return true;

    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL;
}

// ===== 延迟归还：扫描并归还全空的 span =====

void CentralCache::performDelayedReturn(size_t index)
{
    delayCounts_[index].store(0, std::memory_order_relaxed);
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // 遍历中央链表，统计每个 span 现在有多少空闲块
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);

    while (currentBlock)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
            spanFreeCounts[tracker]++;
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    // 更新每个 span 的空闲计数，全空的归还 PageCache
    for (const auto& [tracker, newFreeBlocks] : spanFreeCounts)
    {
        updateSpanFreeCount(tracker, newFreeBlocks, index);
    }
}

// ===== 更新 span 空闲计数，全空则归还 =====

void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index)
{
    size_t oldFreeCount = tracker->freeCount.load(std::memory_order_relaxed);
    size_t newFreeCount = oldFreeCount + newFreeBlocks;
    tracker->freeCount.store(newFreeCount, std::memory_order_release);

    // 如果所有块都空闲了，把整个 span 从中央链表摘掉，归还给 PageCache
    if (newFreeCount == tracker->blockCount.load(std::memory_order_relaxed))
    {
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        // —— 从中央链表中摘掉属于这个 span 的所有块 ——
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = nullptr;
        void* prev = nullptr;
        void* current = head;

        while (current)
        {
            void* next = *reinterpret_cast<void**>(current);
            // 判断 current 是不是在这个 span 范围内
            if (current >= spanAddr &&
                current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
            {
                // 属于这个 span，跳过（相当于从链表摘除）
                if (prev)
                    *reinterpret_cast<void**>(prev) = next;
                else
                    newHead = next;  // 头节点被摘，新头是下一个
            }
            else
            {
                // 不属于，保留
                prev = current;
            }
            current = next;
        }

        centralFreeList_[index].store(newHead, std::memory_order_release);
        // 把整块 span 还给 PageCache（会尝试合并相邻空闲 span）
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}

// ===== 从 PageCache 拿 span =====

void* CentralCache::fetchFromPageCache(size_t size)
{
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // 小对象：固定拿 8 页（32KB），一次切很多块，减少申请次数
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // 大对象：按需拿
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

// ===== 根据块地址找 SpanTracker =====
//
// 线性扫描所有 tracker，找到地址范围包含 blockAddr 的那个
// v2 的性能瓶颈：O(n)，n = span 总数
// v3 用页表（PageTag）改成 O(1)

SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    std::lock_guard<std::mutex> lock(trackerMutex_);
    for (auto& tracker : spanTrackers_)
    {
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        if (blockAddr >= spanAddr &&
            blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
        {
            return tracker.get();
        }
    }
    return nullptr;
}

} // namespace memoryPool
