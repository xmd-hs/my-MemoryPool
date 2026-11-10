// 中央缓存（CentralCache）—— 三层架构的第二层，线程间共享
//
// 职责：
//   - 按 size class 维护空闲链表，所有线程共享
//   - ThreadCache 本地没了，从这里批量取
//   - ThreadCache 本地太多，归还到这里
//   - 自己没了，从 PageCache 拿 span，切成小块串成链表
//   - 用 SpanTracker 追踪每个 span 的空闲块数，全部空闲就归还给 PageCache
//
// 同步：
//   - 每个 size class 一把自旋锁（locks_），临界区短，自旋比 mutex 快
//   - spanTracker 用 mutex 保护（vector 操作，临界区长）
//
// 延迟归还：
//   - 不是每次归还都检查 span 是否全空（遍历链表太贵）
//   - 累积 MAX_DELAY_COUNT 次归还，或距上次检查超过 DELAY_INTERVAL，才统一扫描

#pragma once
#include "Common.h"
#include <mutex>
#include <memory>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>
#include <unordered_map>

namespace xmd_memoryPool
{

// Span 追踪器：记录从 PageCache 拿的一整块内存（span）的信息
// 作用：当 span 里的所有块都归还了，把整个 span 还给 PageCache
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};    // span 起始地址
    std::atomic<size_t> numPages{0};         // span 占多少页
    std::atomic<size_t> blockCount{0};       // span 总共多少块
    std::atomic<size_t> freeCount{0};        // 当前空闲多少块（== blockCount 说明可归还）
};

class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    // 给 ThreadCache 提供一个块（可能触发从 PageCache 拉 span）
    void* fetchRange(size_t index);
    // 接收 ThreadCache 归还的一批块
    void returnRange(void* start, size_t size, size_t index);

private:
    CentralCache();

    // 从 PageCache 拿一个 span，切成小块串成链表
    void* fetchFromPageCache(size_t size);

    // 根据块地址找到它属于哪个 span
    SpanTracker* getSpanTracker(void* blockAddr);

    // 更新 span 空闲计数，若全空则归还 PageCache
    void updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index);

private:
    // 每个 size class 的空闲链表头（原子指针）
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    // 每个 size class 一把自旋锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    // 所有 span 的追踪器（动态数组，避免固定上限）
    std::mutex trackerMutex_;
    std::vector<std::unique_ptr<SpanTracker>> spanTrackers_;

    // —— 延迟归还相关 ——
    static const size_t MAX_DELAY_COUNT = 48;                          // 累积多少次归还后触发检查
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;       // 各档位的归还计数
    std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE>
        lastReturnTimes_;                                                // 各档位上次检查时间
    static const std::chrono::milliseconds DELAY_INTERVAL;              // 距上次检查超过多久也触发

    bool shouldPerformDelayedReturn(size_t index, size_t currentCount,
                                    std::chrono::steady_clock::time_point currentTime);
    void performDelayedReturn(size_t index);
};

} // namespace memoryPool
