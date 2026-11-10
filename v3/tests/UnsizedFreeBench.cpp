#include <xmd/MemoryPool.h>
#include <chrono>
#include <iostream>
#include <vector>
int main()
{
    for (size_t size : {size_t(64), size_t(8192), size_t(65536),
                        size_t(131072), size_t(524288), size_t(2097152)})
    {
        std::vector<void*> blocks(64);
        for (auto& p : blocks)
        {
            p = xmd_memoryPool::MemoryPool::allocate(size);
            if (!p) return 1;
        }
        for (auto p : blocks) xmd_memoryPool::MemoryPool::deallocate(p);
        const auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < 20000; ++round)
        {
            for (auto& p : blocks)
            {
                p = xmd_memoryPool::MemoryPool::allocate(size);
                if (!p) return 1;
            }
            for (auto p : blocks) xmd_memoryPool::MemoryPool::deallocate(p);
        }
        const double ns = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start).count() / 1280000;
        std::cout << size << " " << ns << "\n";
        xmd_memoryPool::MemoryPool::releaseUnused();
    }
}
