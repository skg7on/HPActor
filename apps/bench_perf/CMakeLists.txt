add_executable(16_bench_perf 16_bench_perf.cpp)
target_link_libraries(16_bench_perf PRIVATE hpactor_lib)
target_include_directories(16_bench_perf PRIVATE ${CMAKE_SOURCE_DIR})
