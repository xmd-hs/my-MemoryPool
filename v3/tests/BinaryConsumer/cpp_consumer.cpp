#include <xmd/Allocator.h>
#include <xmd/MemoryPool.h>

#include <vector>

int main()
{
    void* block = xmd_memoryPool::MemoryPool::allocate(512 * 1024);
    if (!block)
        return 1;
    static_cast<char*>(block)[0] = 7;
    xmd_memoryPool::MemoryPool::deallocate(block, 512 * 1024);

    std::vector<int, xmd_memoryPool::Allocator<int>> values(128, 3);
    return values.size() == 128 && values[0] == 3 ? 0 : 2;
}
