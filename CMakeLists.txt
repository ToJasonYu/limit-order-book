cmake_minimum_required(VERSION 3.16)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Benchmarks stay off by default until benchmarks/CMakeLists.txt exists.
option(LOB_BUILD_TESTS "Build the Catch2 test suite" ON)
option(LOB_BUILD_BENCHMARKS "Build the throughput benchmarks" OFF)

# The order book / matching engine itself, as a static library. Everything
# else (the CLI driver, tests, benchmarks) links against this so there's a
# single compiled definition of the core logic.
add_library(lob STATIC
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(lob PUBLIC include)

if(MSVC)
    target_compile_options(lob PRIVATE /W4)
else()
    target_compile_options(lob PRIVATE -Wall -Wextra -Wpedantic)
endif()

# CLI driver: replays a CSV/stdin sequence of orders through the book and
# prints trades. See src/main.cpp for the file format.
add_executable(lob_cli src/main.cpp)
target_link_libraries(lob_cli PRIVATE lob)
if(MSVC)
    target_compile_options(lob_cli PRIVATE /W4)
else()
    target_compile_options(lob_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LOB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LOB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
