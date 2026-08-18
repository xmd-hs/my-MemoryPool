#pragma once

#include <cstddef>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

namespace Kama_memoryPool
{

// 向操作系统申请按页对齐的内存。匿名页本身已清零，无需再 memset。
inline void* allocPages(size_t size)
{
    if (size == 0)
        return nullptr;

#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return nullptr;
    return ptr;
#endif
}

inline void freePages(void* ptr, size_t size)
{
    if (!ptr)
        return;

#ifdef _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

} // namespace Kama_memoryPool
