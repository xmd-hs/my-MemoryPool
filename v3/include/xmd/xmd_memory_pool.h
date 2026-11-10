#pragma once

#include "export.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

XMD_API void* xmd_alloc(size_t size);
XMD_API void* xmd_alloc_aligned(size_t size, size_t alignment);
XMD_API void  xmd_free(void* ptr);
XMD_API void  xmd_free_sized(void* ptr, size_t size);
XMD_API void  xmd_trim(void);
XMD_API void  xmd_set_page_cache_limit(size_t bytes);
XMD_API size_t xmd_page_cache_limit(void);
XMD_API void  xmd_set_large_cache_limit(size_t bytes);
XMD_API size_t xmd_large_cache_limit(void);
XMD_API void  xmd_release_unused(void);

#ifdef __cplusplus
}
#endif
