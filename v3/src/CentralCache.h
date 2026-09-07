#pragma once
#include "Common.h"
#include "PageCache.h"

#include <array>
#include <atomic>
#include <vector>

namespace Kama_memoryPool
{

constexpr size_t kCentralShardCount = 8;

struct CacheShard
{
    alignas(64) std::atomic_flag lock;
    Span* partial;
};

// Remote frees are updated by arbitrary producer threads. Keep each head in
// its own cache line so unrelated size classes / shards do not false-share.
struct alignas(64) RemoteFreeList
{
    std::atomic<void*> head{nullptr};
};

struct SpanFreeGroup
{
    Span* span = nullptr;
    void* head = nullptr;
};

// Cross-thread magazine: already-checked-out blocks that survive thread exit.
// Empty check is one relaxed load; cap is small so this cannot bloat RSS.
struct alignas(64) TransferBin
{
    std::atomic<void*> head{nullptr};
    std::atomic<size_t> count{0};
};

class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index, size_t batchNum, size_t& fetchedCount);
    void returnRange(void* start, size_t blockCount, size_t index);
    void pushRemoteFree(void* ptr, size_t index);
    void pushRemoteFreeRange(void* head, size_t count, size_t index);
    void pushTransfer(void* head, size_t count, size_t index);

private:
    CentralCache();
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    enum class ReturnAction
    {
        None,
        FullSpan,
        Retry
    };

    size_t localShardId() const;
    size_t shardIndex(size_t sizeClassIndex, size_t shardId) const;

    Span* createSpan(size_t index);
    void unlinkPartial(size_t shardIndex, Span* span);
    void mergeRemoteFrees(size_t index, size_t shardId);
    size_t popRangeFromShardLocked(size_t shardIdx, size_t limit,
                                   void*& head, void*& tail);
    size_t popTransfer(size_t index, size_t limit, void*& head, void*& tail);
    size_t transferCap(size_t index) const;
    ReturnAction returnSpanGroupLocked(Span* span, void* head, size_t shardIndex, size_t sizeClassIndex);
    void returnGroups(size_t index, size_t preferredShardId,
                      const std::vector<SpanFreeGroup>& groups);
    void collectGroups(void* list, size_t index, std::vector<SpanFreeGroup>& groups);

    std::array<CacheShard, FREE_LIST_SIZE * kCentralShardCount> shards_{};
    std::array<RemoteFreeList, FREE_LIST_SIZE * kCentralShardCount> remoteFreeLists_{};
    std::array<std::atomic<std::uint8_t>, FREE_LIST_SIZE> remotePending_{};
    std::array<TransferBin, FREE_LIST_SIZE> transfer_{};
};

} // namespace Kama_memoryPool
