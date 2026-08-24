# Benchmarks are built with the same flags as the rest of the project, but
# should always be *run* from a Release configuration (see BENCHMARKS.md) --
# a Debug build's unoptimized code and (with some standard library
# implementations) checked-iterator overhead would produce numbers that
# don't mean anything.

add_executable(bench_throughput benchmark_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE lob)

add_executable(bench_naive_comparison benchmark_naive_comparison.cpp)
target_link_libraries(bench_naive_comparison PRIVATE lob)

if(MSVC)
    target_compile_options(bench_throughput PRIVATE /W4)
    target_compile_options(bench_naive_comparison PRIVATE /W4)
else()
    target_compile_options(bench_throughput PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(bench_naive_comparison PRIVATE -Wall -Wextra -Wpedantic)
endif()
