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

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

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

set_target_properties(kama_memory_pool PROPERTIES
    OUTPUT_NAME kama_memory_pool
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME memory_pool
)

if(KAMA_MEMORY_POOL_BUILD_TESTS)
    add_executable(unit_test tests/UnitTest.cpp)
    add_executable(perf_test tests/PerformanceTest.cpp)
    target_link_libraries(unit_test PRIVATE kama_memory_pool)
    target_link_libraries(perf_test PRIVATE kama_memory_pool)
    target_include_directories(unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_custom_target(test
        COMMAND $<TARGET_FILE:unit_test>
        DEPENDS unit_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    add_custom_target(perf
        COMMAND $<TARGET_FILE:perf_test>
        DEPENDS perf_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
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
