cmake_minimum_required(VERSION 3.14)
project(KamaMemoryPool VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(KAMA_MEMORY_POOL_BUILD_SHARED "Build shared library" OFF)
option(KAMA_MEMORY_POOL_BUILD_TESTS "Build unit and performance tests" ON)
option(KAMA_MEMORY_POOL_BUILD_EXAMPLES "Build usage examples" ON)
option(KAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS "Enable debug validation for frees" OFF)
option(KAMA_MEMORY_POOL_ENABLE_STATS "Enable global allocation statistics (adds hot-path atomics)" OFF)
option(KAMA_MEMORY_POOL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(KAMA_MEMORY_POOL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(KAMA_MEMORY_POOL_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

if(KAMA_MEMORY_POOL_ENABLE_TSAN AND (KAMA_MEMORY_POOL_ENABLE_ASAN OR KAMA_MEMORY_POOL_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan/UBSan")
endif()

set(KAMA_SANITIZER_FLAGS)
if(NOT MSVC)
    if(KAMA_MEMORY_POOL_ENABLE_ASAN)
        list(APPEND KAMA_SANITIZER_FLAGS -fsanitize=address)
    endif()
    if(KAMA_MEMORY_POOL_ENABLE_UBSAN)
        list(APPEND KAMA_SANITIZER_FLAGS -fsanitize=undefined)
    endif()
    if(KAMA_MEMORY_POOL_ENABLE_TSAN)
        list(APPEND KAMA_SANITIZER_FLAGS -fsanitize=thread)
    endif()
endif()

set(KAMA_SRC
    src/MemoryPool.cpp
    src/kama_c_api.cpp
    src/ThreadCache.cpp
    src/CentralCache.cpp
    src/PageCache.cpp
)

if(KAMA_MEMORY_POOL_BUILD_SHARED)
    add_library(kama_memory_pool SHARED ${KAMA_SRC})
    target_compile_definitions(kama_memory_pool
        PUBLIC KAMA_MEMORY_POOL_SHARED
        PRIVATE KAMA_MEMORY_POOL_BUILDING
    )
    set_target_properties(kama_memory_pool PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
else()
    add_library(kama_memory_pool STATIC ${KAMA_SRC})
endif()

add_library(KamaMemoryPool::memory_pool ALIAS kama_memory_pool)

target_include_directories(kama_memory_pool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(kama_memory_pool PUBLIC Threads::Threads)
if(KAMA_MEMORY_POOL_ENABLE_DEBUG_GUARDS)
    target_compile_definitions(kama_memory_pool PUBLIC KAMA_MEMORY_POOL_DEBUG_GUARDS=1)
endif()
if(KAMA_MEMORY_POOL_ENABLE_STATS)
    target_compile_definitions(kama_memory_pool PUBLIC KAMA_MEMORY_POOL_STATS_ENABLED=1)
else()
    target_compile_definitions(kama_memory_pool PUBLIC KAMA_MEMORY_POOL_STATS_ENABLED=0)
endif()
if(KAMA_SANITIZER_FLAGS)
    target_compile_options(kama_memory_pool PUBLIC ${KAMA_SANITIZER_FLAGS})
    target_link_options(kama_memory_pool PUBLIC ${KAMA_SANITIZER_FLAGS})
endif()

set_target_properties(kama_memory_pool PROPERTIES
    OUTPUT_NAME kama_memory_pool
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME memory_pool
)

if(KAMA_MEMORY_POOL_BUILD_TESTS)
    enable_testing()
    add_executable(unit_test tests/UnitTest.cpp)
    add_executable(perf_test tests/PerformanceTest.cpp)
    target_link_libraries(unit_test PRIVATE kama_memory_pool)
    target_link_libraries(perf_test PRIVATE kama_memory_pool)
    target_include_directories(unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(KAMA_SANITIZER_FLAGS)
        target_compile_options(unit_test PRIVATE ${KAMA_SANITIZER_FLAGS})
        target_compile_options(perf_test PRIVATE ${KAMA_SANITIZER_FLAGS})
        target_link_options(unit_test PRIVATE ${KAMA_SANITIZER_FLAGS})
        target_link_options(perf_test PRIVATE ${KAMA_SANITIZER_FLAGS})
    endif()

    add_test(NAME kama_unit_test COMMAND $<TARGET_FILE:unit_test>)
    add_test(NAME kama_perf_smoke COMMAND $<TARGET_FILE:perf_test>)

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
    option(KAMA_MEMORY_POOL_USE_BENCHMARK "Build Google Benchmark based tests" OFF)
    if(KAMA_MEMORY_POOL_USE_BENCHMARK)
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
        target_link_libraries(bench_test PRIVATE kama_memory_pool benchmark::benchmark)
        if(KAMA_SANITIZER_FLAGS)
            target_compile_options(bench_test PRIVATE ${KAMA_SANITIZER_FLAGS})
            target_link_options(bench_test PRIVATE ${KAMA_SANITIZER_FLAGS})
        endif()
    endif()
endif()

if(KAMA_MEMORY_POOL_BUILD_EXAMPLES)
    add_executable(kama_example examples/basic.cpp)
    target_link_libraries(kama_example PRIVATE kama_memory_pool)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS kama_memory_pool
    EXPORT KamaMemoryPoolTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/kama
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

configure_package_config_file(
    cmake/KamaMemoryPoolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/KamaMemoryPoolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KamaMemoryPool
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/KamaMemoryPoolConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/KamaMemoryPoolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/KamaMemoryPoolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KamaMemoryPool
)

install(EXPORT KamaMemoryPoolTargets
    FILE KamaMemoryPoolTargets.cmake
    NAMESPACE KamaMemoryPool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KamaMemoryPool
)
