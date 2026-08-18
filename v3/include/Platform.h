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
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

namespace Kama_memoryPool
{

inline size_t systemPageSize()
{
    static const size_t cached = []() -> size_t {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwPageSize ? static_cast<size_t>(info.dwPageSize) : 4096;
#else
        long page = sysconf(_SC_PAGESIZE);
        return page > 0 ? static_cast<size_t>(page) : 4096;
#endif
    }();
    return cached;
}

inline size_t systemAllocGranularity()
{
#ifdef _WIN32
    static const size_t cached = []() -> size_t {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwAllocationGranularity
                   ? static_cast<size_t>(info.dwAllocationGranularity)
                   : systemPageSize();
    }();
    return cached;
#else
    return systemPageSize();
#endif
}

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
