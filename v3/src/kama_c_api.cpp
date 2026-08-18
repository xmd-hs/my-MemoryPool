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

}
