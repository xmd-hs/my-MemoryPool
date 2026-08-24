#pragma once
#include "Common.h"
#include "PageCache.h"

#include <array>
#include <atomic>
#include <unordered_map>

namespace Kama_memoryPool
{

constexpr size_t kCentralShardCount = 8;

struct CacheShard
{
    alignas(64) std::atomic_flag lock;
    Span* partial;
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
    void* popOneFromShardLocked(size_t shardIndex);
    ReturnAction returnSpanGroupLocked(Span* span, void* head, size_t shardIndex, size_t sizeClassIndex);
    void returnGroups(size_t index, size_t preferredShardId,
                      const std::unordered_map<Span*, void*>& groups);

    std::array<CacheShard, FREE_LIST_SIZE * kCentralShardCount> shards_{};
    std::array<std::atomic<void*>, FREE_LIST_SIZE> remoteFreeLists_{};
};

} // namespace Kama_memoryPool
