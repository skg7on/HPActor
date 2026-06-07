add_executable(15_cli_demo 15_cli_demo.cpp)
target_link_libraries(15_cli_demo PRIVATE hpactor_lib)
target_include_directories(15_cli_demo PRIVATE ${CMAKE_SOURCE_DIR})
