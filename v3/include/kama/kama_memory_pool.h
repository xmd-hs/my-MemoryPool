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

#ifdef __cplusplus
}
#endif
