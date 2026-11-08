#include <kama/Allocator.h>
#include <kama/MemoryPool.h>

#include <vector>

int main()
{
    void* block = Kama_memoryPool::MemoryPool::allocate(512 * 1024);
    if (!block)
        return 1;
    static_cast<char*>(block)[0] = 7;
    Kama_memoryPool::MemoryPool::deallocate(block, 512 * 1024);

    std::vector<int, Kama_memoryPool::Allocator<int>> values(128, 3);
    return values.size() == 128 && values[0] == 3 ? 0 : 2;
}
