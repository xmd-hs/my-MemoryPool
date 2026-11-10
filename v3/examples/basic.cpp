#include <xmd/Allocator.h>
#include <xmd/MemoryPool.h>
#include <xmd/xmd_memory_pool.h>

#include <iostream>
#include <vector>

int main()
{
    using xmd_memoryPool::MemoryPool;
    using xmd_memoryPool::Allocator;

    void* p = MemoryPool::allocate(64);
    if (!p)
    {
        std::cerr << "allocate failed\n";
        return 1;
    }
    static_cast<char*>(p)[0] = 42;
    MemoryPool::deallocate(p);

    int* n = MemoryPool::newElement<int>(7);
    MemoryPool::deleteElement(n);

    std::vector<int, Allocator<int>> nums;
    nums.push_back(1);
    nums.push_back(2);

    void* q = xmd_alloc(32);
    xmd_free(q);

    auto st = MemoryPool::stats();
    std::cout << "xmd memory pool sdk ok, alloc=" << st.allocCount
              << " free=" << st.freeCount << "\n";
    return 0;
}
