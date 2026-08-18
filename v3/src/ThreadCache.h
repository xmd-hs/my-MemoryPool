#pragma once
#include "Common.h"
#include <array>

namespace Kama_memoryPool
{

class ThreadCache
{
public:
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance;
        return &instance;
    }

    void* allocate(size_t size);
    void* allocateAligned(size_t size, size_t alignment);
    bool deallocate(void* ptr);
    bool deallocate(void* ptr, size_t size);

private:
    ThreadCache();
    ~ThreadCache();

    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;

    void* fetchFromCentralCache(size_t index);
    void returnToCentralCache(size_t index);
    void flushAll();
    size_t getBatchNum(size_t size) const;
    size_t maxCachedBlocks(size_t size) const;
    bool shouldReturnToCentralCache(size_t index) const;

    std::array<void*, FREE_LIST_SIZE> freeList_{};
    std::array<size_t, FREE_LIST_SIZE> freeListSize_{};
};

} // namespace Kama_memoryPool
