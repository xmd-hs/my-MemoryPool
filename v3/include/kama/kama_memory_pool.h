#pragma once

#include "export.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

KAMA_API void* kama_alloc(size_t size);
KAMA_API void* kama_alloc_aligned(size_t size, size_t alignment);
KAMA_API void  kama_free(void* ptr);
KAMA_API void  kama_free_sized(void* ptr, size_t size);
KAMA_API void  kama_trim(void);
KAMA_API void  kama_set_page_cache_limit(size_t bytes);
KAMA_API size_t kama_page_cache_limit(void);
KAMA_API void  kama_set_large_cache_limit(size_t bytes);
KAMA_API size_t kama_large_cache_limit(void);
KAMA_API void  kama_release_unused(void);

#ifdef __cplusplus
}
#endif
