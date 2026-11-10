#pragma once

#if defined(XMD_MEMORY_POOL_SHARED)
#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef XMD_MEMORY_POOL_BUILDING
#define XMD_API __declspec(dllexport)
#else
#define XMD_API __declspec(dllimport)
#endif
#else
#define XMD_API __attribute__((visibility("default")))
#endif
#else
#define XMD_API
#endif

#define XMD_MEMORY_POOL_VERSION_MAJOR 2
#define XMD_MEMORY_POOL_VERSION_MINOR 0
#define XMD_MEMORY_POOL_VERSION_PATCH 0
#define XMD_MEMORY_POOL_VERSION "2.0.0"
