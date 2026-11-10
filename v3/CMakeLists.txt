cmake_minimum_required(VERSION 3.14)
project(xmdMemoryPool VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(XMD_MEMORY_POOL_BUILD_SHARED "Build shared library" OFF)
option(XMD_MEMORY_POOL_BUILD_TESTS "Build unit and performance tests" ON)
option(XMD_MEMORY_POOL_BUILD_EXAMPLES "Build usage examples" ON)
option(XMD_MEMORY_POOL_ENABLE_DEBUG_GUARDS "Enable debug validation for frees" OFF)
option(XMD_MEMORY_POOL_ENABLE_STATS "Enable global allocation statistics (adds hot-path atomics)" OFF)
option(XMD_MEMORY_POOL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(XMD_MEMORY_POOL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(XMD_MEMORY_POOL_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(XMD_MEMORY_POOL_PROFILE "Enable low-overhead allocation diagnostics" OFF)

if(XMD_MEMORY_POOL_PROFILE)
    set(XMD_MEMORY_POOL_ENABLE_STATS ON CACHE BOOL "Enable global allocation statistics" FORCE)
    message(STATUS "MemoryPool profile diagnostics enabled (stats on)")
endif()

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Release LTO/LTCG so MemoryPool::allocate can inline into perf_test / user TUs.
# Skip when sanitizers are on — ASan+LTO is fragile and not a perf configuration.
if(NOT XMD_MEMORY_POOL_ENABLE_ASAN AND NOT XMD_MEMORY_POOL_ENABLE_UBSAN AND NOT XMD_MEMORY_POOL_ENABLE_TSAN)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT XMD_IPO_SUPPORTED OUTPUT XMD_IPO_ERROR)
    if(XMD_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        message(STATUS "Release IPO/LTO enabled")
    else()
        message(STATUS "Release IPO/LTO not enabled: ${XMD_IPO_ERROR}")
    endif()
endif()

find_package(Threads REQUIRED)

if(XMD_MEMORY_POOL_ENABLE_TSAN AND (XMD_MEMORY_POOL_ENABLE_ASAN OR XMD_MEMORY_POOL_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan/UBSan")
endif()

set(XMD_SANITIZER_FLAGS)
if(NOT MSVC)
    if(XMD_MEMORY_POOL_ENABLE_ASAN)
        list(APPEND XMD_SANITIZER_FLAGS -fsanitize=address)
    endif()
    if(XMD_MEMORY_POOL_ENABLE_UBSAN)
        list(APPEND XMD_SANITIZER_FLAGS -fsanitize=undefined)
    endif()
    if(XMD_MEMORY_POOL_ENABLE_TSAN)
        list(APPEND XMD_SANITIZER_FLAGS -fsanitize=thread)
    endif()
endif()

set(XMD_SRC
    src/MemoryPool.cpp
    src/xmd_c_api.cpp
    src/ThreadCache.cpp
    src/CentralCache.cpp
    src/PageCache.cpp
)

if(XMD_MEMORY_POOL_BUILD_SHARED)
    add_library(xmd_memory_pool SHARED ${XMD_SRC})
    target_compile_definitions(xmd_memory_pool
        PUBLIC XMD_MEMORY_POOL_SHARED
        PRIVATE XMD_MEMORY_POOL_BUILDING
    )
    set_target_properties(xmd_memory_pool PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
else()
    add_library(xmd_memory_pool STATIC ${XMD_SRC})
endif()

add_library(xmdMemoryPool::memory_pool ALIAS xmd_memory_pool)
target_compile_features(xmd_memory_pool PUBLIC cxx_std_14)

target_include_directories(xmd_memory_pool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(xmd_memory_pool PUBLIC Threads::Threads)
if(XMD_MEMORY_POOL_ENABLE_DEBUG_GUARDS)
    target_compile_definitions(xmd_memory_pool PUBLIC XMD_MEMORY_POOL_DEBUG_GUARDS=1)
endif()
if(XMD_MEMORY_POOL_ENABLE_STATS)
    target_compile_definitions(xmd_memory_pool PUBLIC XMD_MEMORY_POOL_STATS_ENABLED=1)
else()
    target_compile_definitions(xmd_memory_pool PUBLIC XMD_MEMORY_POOL_STATS_ENABLED=0)
endif()
if(XMD_SANITIZER_FLAGS)
    target_compile_options(xmd_memory_pool PUBLIC ${XMD_SANITIZER_FLAGS})
    target_link_options(xmd_memory_pool PUBLIC ${XMD_SANITIZER_FLAGS})
endif()

set_target_properties(xmd_memory_pool PROPERTIES
    OUTPUT_NAME xmd_memory_pool
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME memory_pool
)
if(XMD_IPO_SUPPORTED)
    set_property(TARGET xmd_memory_pool PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if(XMD_MEMORY_POOL_BUILD_TESTS)
    enable_testing()
    add_executable(unit_test tests/UnitTest.cpp)
    add_executable(perf_test tests/PerformanceTest.cpp)
    add_executable(workload_bench tests/WorkloadBench.cpp)
    add_executable(unsized_free_bench tests/UnsizedFreeBench.cpp)
    target_link_libraries(unsized_free_bench PRIVATE xmd_memory_pool)
    if(XMD_MEMORY_POOL_BUILD_SHARED)
        # Internal tests compile their own implementation; SDK symbols stay hidden.
        target_sources(unit_test PRIVATE ${XMD_SRC})
        target_include_directories(unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(unit_test PRIVATE
            XMD_MEMORY_POOL_STATS_ENABLED=$<BOOL:${XMD_MEMORY_POOL_ENABLE_STATS}>
            XMD_MEMORY_POOL_DEBUG_GUARDS=$<BOOL:${XMD_MEMORY_POOL_ENABLE_DEBUG_GUARDS}>)
        target_link_libraries(unit_test PRIVATE Threads::Threads)
    else()
        target_link_libraries(unit_test PRIVATE xmd_memory_pool)
    endif()
    target_link_libraries(perf_test PRIVATE xmd_memory_pool)
    target_link_libraries(workload_bench PRIVATE xmd_memory_pool)
    if(XMD_IPO_SUPPORTED)
        set_property(TARGET unit_test PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET perf_test PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET workload_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
    target_include_directories(unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(XMD_SANITIZER_FLAGS)
        target_compile_options(unit_test PRIVATE ${XMD_SANITIZER_FLAGS})
        target_compile_options(perf_test PRIVATE ${XMD_SANITIZER_FLAGS})
        target_compile_options(workload_bench PRIVATE ${XMD_SANITIZER_FLAGS})
        target_link_options(unit_test PRIVATE ${XMD_SANITIZER_FLAGS})
        target_link_options(perf_test PRIVATE ${XMD_SANITIZER_FLAGS})
        target_link_options(workload_bench PRIVATE ${XMD_SANITIZER_FLAGS})
    endif()

    add_test(NAME xmd_unit_test COMMAND $<TARGET_FILE:unit_test>)
    # Performance ratios fluctuate with scheduling; run the report in CTest
    # without making a 5% microbenchmark threshold a correctness gate.
    add_test(NAME xmd_perf_smoke COMMAND $<TARGET_FILE:perf_test> --report)

    add_custom_target(run_unit_tests
        COMMAND $<TARGET_FILE:unit_test>
        DEPENDS unit_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    add_custom_target(perf
        COMMAND $<TARGET_FILE:perf_test>
        DEPENDS perf_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )

    # Google Benchmark 性能测试
    option(XMD_MEMORY_POOL_USE_BENCHMARK "Build Google Benchmark based tests" OFF)
    if(XMD_MEMORY_POOL_USE_BENCHMARK)
        include(FetchContent)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE INTERNAL "")
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE INTERNAL "")
        FetchContent_Declare(
            googlebench
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.8.5
        )
        FetchContent_MakeAvailable(googlebench)

        add_executable(bench_test tests/BenchmarkTest.cpp)
        target_link_libraries(bench_test PRIVATE xmd_memory_pool benchmark::benchmark)
        if(XMD_SANITIZER_FLAGS)
            target_compile_options(bench_test PRIVATE ${XMD_SANITIZER_FLAGS})
            target_link_options(bench_test PRIVATE ${XMD_SANITIZER_FLAGS})
        endif()
    endif()
endif()

if(XMD_MEMORY_POOL_BUILD_EXAMPLES)
    add_executable(xmd_example examples/basic.cpp)
    target_link_libraries(xmd_example PRIVATE xmd_memory_pool)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS xmd_memory_pool
    EXPORT xmdMemoryPoolTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/xmd
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

configure_package_config_file(
    cmake/xmdMemoryPoolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/xmdMemoryPoolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xmdMemoryPool
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/xmdMemoryPoolConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/xmdMemoryPoolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/xmdMemoryPoolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xmdMemoryPool
)

install(EXPORT xmdMemoryPoolTargets
    FILE xmdMemoryPoolTargets.cmake
    NAMESPACE xmdMemoryPool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xmdMemoryPool
)

if(XMD_MEMORY_POOL_BUILD_SHARED)
    set(XMD_PACKAGE_LINKAGE shared)
else()
    set(XMD_PACKAGE_LINKAGE static)
endif()
set(CPACK_PACKAGE_NAME xmdMemoryPool)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "xmdMemoryPool-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_CXX_COMPILER_ID}-${XMD_PACKAGE_LINKAGE}")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND XMD_MEMORY_POOL_BUILD_SHARED)
    set(CPACK_STRIP_FILES TRUE)
endif()
include(CPack)
