add_executable(18_bench_caf
    18_bench_caf.cpp
)
target_link_libraries(18_bench_caf PRIVATE hpactor_lib)
target_include_directories(18_bench_caf PRIVATE ${CMAKE_SOURCE_DIR})
