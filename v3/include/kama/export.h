#pragma once

#if defined(KAMA_MEMORY_POOL_SHARED)
#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef KAMA_MEMORY_POOL_BUILDING
#define KAMA_API __declspec(dllexport)
#else
#define KAMA_API __declspec(dllimport)
#endif
#else
#define KAMA_API __attribute__((visibility("default")))
#endif
#else
#define KAMA_API
#endif

#define KAMA_MEMORY_POOL_VERSION_MAJOR 1
#define KAMA_MEMORY_POOL_VERSION_MINOR 0
#define KAMA_MEMORY_POOL_VERSION_PATCH 0
#define KAMA_MEMORY_POOL_VERSION "1.0.0"
