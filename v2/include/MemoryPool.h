// MemoryPool 门面：对外只暴露 allocate / deallocate 两个接口
// 实际工作全部委托给 ThreadCache（第一层），由它按需向下走 CentralCache → PageCache

#pragma once
#include "ThreadCache.h"

namespace xmd_memoryPool
{

class MemoryPool
{
public:
    static void* allocate(size_t size)
    {
        return ThreadCache::getInstance()->allocate(size);
    }

    static void deallocate(void* ptr, size_t size)
    {
        ThreadCache::getInstance()->deallocate(ptr, size);
    }
};

} // namespace memoryPool
