add_executable(14_edgeops_telemetry
    14_edgeops_telemetry.cpp
    scenario.cpp
)
target_link_libraries(14_edgeops_telemetry PRIVATE hpactor_lib)
target_include_directories(14_edgeops_telemetry PRIVATE ${CMAKE_SOURCE_DIR})
