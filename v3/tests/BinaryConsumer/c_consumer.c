#include <kama/kama_memory_pool.h>

int main(void)
{
    unsigned char* block = (unsigned char*)kama_alloc(128);
    if (!block)
        return 1;
    block[0] = 7;
    kama_free_sized(block, 128);
    kama_release_unused();
    return 0;
}
