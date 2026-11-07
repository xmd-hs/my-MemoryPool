// C ABI 实现（v3）
//
// 薄包装，把 C 接口委托给 MemoryPool 门面。

#include <kama/kama_memory_pool.h>
#include <kama/MemoryPool.h>

extern "C" {

void* kama_alloc(size_t size)
{
    return Kama_memoryPool::MemoryPool::allocate(size);
}

void* kama_alloc_aligned(size_t size, size_t alignment)
{
    return Kama_memoryPool::MemoryPool::allocateAligned(size, alignment);
}

void kama_free(void* ptr)
{
    Kama_memoryPool::MemoryPool::deallocate(ptr);
}

void kama_free_sized(void* ptr, size_t size)
{
    Kama_memoryPool::MemoryPool::deallocate(ptr, size);
}

void kama_trim(void)
{
    Kama_memoryPool::MemoryPool::trim();
}

void kama_set_page_cache_limit(size_t bytes)
{
    Kama_memoryPool::MemoryPool::setPageCacheLimit(bytes);
}

size_t kama_page_cache_limit(void)
{
    return Kama_memoryPool::MemoryPool::pageCacheLimit();
}

void kama_set_large_cache_limit(size_t bytes)
{
    Kama_memoryPool::MemoryPool::setLargeCacheLimit(bytes);
}

size_t kama_large_cache_limit(void)
{
    return Kama_memoryPool::MemoryPool::largeCacheLimit();
}

void kama_release_unused(void)
{
    Kama_memoryPool::MemoryPool::releaseUnused();
}

}
