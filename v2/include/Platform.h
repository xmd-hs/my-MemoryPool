// 平台层：屏蔽 Windows / Linux 的内存申请差异
//   Windows：VirtualAlloc / VirtualFree
//   Linux：mmap / munmap
//
// 返回的内存都是页对齐（4KB）的，适合 PageCache 直接用

#pragma once  // 防止头文件被重复包含

#include <cstddef>  // size_t 等基础类型

#ifdef _WIN32  // Windows 平台
#ifndef WIN32_LEAN_AND_MEAN  // 如果没定义 WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  // 定义它，排除 windows.h 里不常用的内容（加快编译）
#endif
#ifndef NOMINMAX  // 如果没定义 NOMINMAX
#define NOMINMAX  // 定义它，避免 windows.h 的 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>  // Windows API 头文件，提供 VirtualAlloc/VirtualFree
#else  // 非 Windows（Linux/macOS）
#include <sys/mman.h>  // mmap/munmap 系统调用
#ifndef MAP_ANONYMOUS  // 有些老系统叫 MAP_ANON 不叫 MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON  // 统一成 MAP_ANONYMOUS
#endif
#endif

namespace Kama_memoryPool  // 命名空间，避免和其他库的同名函数冲突
{

// 申请 size 字节的页对齐内存（匿名映射，已清零）
inline void* allocPages(size_t size)  // inline 避免多文件重复定义
{
    if (size == 0)  // size 为 0，直接返回空
        return nullptr;

#ifdef _WIN32  // Windows 实现
    // VirtualAlloc：申请虚拟内存
    // nullptr = 让系统选地址
    // size = 申请大小
    // MEM_RESERVE | MEM_COMMIT = 保留并提交（直接占用物理内存）
    // PAGE_READWRITE = 可读可写
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else  // Linux 实现
    // mmap：映射内存
    // nullptr = 让系统选地址
    // size = 映射大小
    // PROT_READ | PROT_WRITE = 可读可写
    // MAP_PRIVATE | MAP_ANONYMOUS = 私有匿名映射（不和其他进程共享，不对应文件）
    // -1, 0 = 匿名映射不需要文件描述符
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)  // mmap 失败返回 MAP_FAILED
        return nullptr;
    return ptr;
#endif
}

// 释放 allocPages 申请的内存
inline void freePages(void* ptr, size_t size)
{
    if (!ptr)  // 空指针直接返回
        return;

#ifdef _WIN32  // Windows 实现
    (void)size;  // VirtualFree 不需要 size，标记为未使用避免警告
    // VirtualFree：释放虚拟内存
    // 0 + MEM_RELEASE = 释放整个分配区域（不需要传大小）
    VirtualFree(ptr, 0, MEM_RELEASE);
#else  // Linux 实现
    munmap(ptr, size);  // munmap 需要传起始地址和大小
#endif
}

} // namespace Kama_memoryPool
