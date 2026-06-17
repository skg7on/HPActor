add_executable(17_bench_saturate
    17_bench_saturate.cpp
    commands/saturate_commands.cpp
)
target_link_libraries(17_bench_saturate PRIVATE hpactor_lib)
target_include_directories(17_bench_saturate PRIVATE ${CMAKE_SOURCE_DIR})
