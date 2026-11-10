#include <xmd/xmd_memory_pool.h>

int main(void)
{
    unsigned char* block = (unsigned char*)xmd_alloc(128);
    if (!block)
        return 1;
    block[0] = 7;
    xmd_free_sized(block, 128);
    xmd_release_unused();
    return 0;
}
