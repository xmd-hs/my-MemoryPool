cmake_minimum_required(VERSION 3.10)
project(memory_pool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -O2)
endif()

find_package(Threads REQUIRED)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)

file(GLOB SOURCES "${SRC_DIR}/*.cpp")

add_executable(unit_test
    ${SOURCES}
    ${TEST_DIR}/UnitTest.cpp
)

add_executable(perf_test
    ${SOURCES}
    ${TEST_DIR}/PerformanceTest.cpp
)

target_include_directories(unit_test PRIVATE ${INC_DIR})
target_include_directories(perf_test PRIVATE ${INC_DIR})

target_link_libraries(unit_test PRIVATE Threads::Threads)
target_link_libraries(perf_test PRIVATE Threads::Threads)

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
