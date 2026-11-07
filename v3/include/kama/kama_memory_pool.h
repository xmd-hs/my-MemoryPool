// C ABI 接口（v3）
//
// 提供 C 语言可调用的接口，方便非 C++ 项目或 C ABI 场景使用。
// 底层调用 MemoryPool 门面。

#pragma once

#include "export.h"  // KAMA_API

#include <stddef.h>   // size_t

#ifdef __cplusplus
extern "C" {  // C++ 编译器按 C 链接生成符号
#endif

KAMA_API void* kama_alloc(size_t size);                       // 分配 size 字节
KAMA_API void* kama_alloc_aligned(size_t size, size_t alignment);  // 按对齐分配
KAMA_API void  kama_free(void* ptr);                          // 释放（不带大小）
KAMA_API void  kama_free_sized(void* ptr, size_t size);       // 释放（带大小）
KAMA_API void  kama_trim(void);                               // 归还当前线程多余缓存
KAMA_API void  kama_set_page_cache_limit(size_t bytes);        // 全局空闲页预算
KAMA_API size_t kama_page_cache_limit(void);
KAMA_API void  kama_set_large_cache_limit(size_t bytes);       // 每线程大对象缓存预算
KAMA_API size_t kama_large_cache_limit(void);
KAMA_API void  kama_release_unused(void);                      // 回收调用线程及当前可回收的全局缓存

#ifdef __cplusplus
}
#endif
