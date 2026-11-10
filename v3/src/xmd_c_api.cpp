// C ABI 实现（v3）
//
// 薄包装，把 C 接口委托给 MemoryPool 门面。

#include <xmd/xmd_memory_pool.h>
#include <xmd/MemoryPool.h>

extern "C" {

void* xmd_alloc(size_t size)
{
    return xmd_memoryPool::MemoryPool::allocate(size);
}

void* xmd_alloc_aligned(size_t size, size_t alignment)
{
    return xmd_memoryPool::MemoryPool::allocateAligned(size, alignment);
}

void xmd_free(void* ptr)
{
    xmd_memoryPool::MemoryPool::deallocate(ptr);
}

void xmd_free_sized(void* ptr, size_t size)
{
    xmd_memoryPool::MemoryPool::deallocate(ptr, size);
}

void xmd_trim(void)
{
    xmd_memoryPool::MemoryPool::trim();
}

void xmd_set_page_cache_limit(size_t bytes)
{
    xmd_memoryPool::MemoryPool::setPageCacheLimit(bytes);
}

size_t xmd_page_cache_limit(void)
{
    return xmd_memoryPool::MemoryPool::pageCacheLimit();
}

void xmd_set_large_cache_limit(size_t bytes)
{
    xmd_memoryPool::MemoryPool::setLargeCacheLimit(bytes);
}

size_t xmd_large_cache_limit(void)
{
    return xmd_memoryPool::MemoryPool::largeCacheLimit();
}

void xmd_release_unused(void)
{
    xmd_memoryPool::MemoryPool::releaseUnused();
}

}
