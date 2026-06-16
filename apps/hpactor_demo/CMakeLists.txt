add_executable(hpactor_demo
    main.cpp
    cli_demo_actor_factory.cpp
    foreground_runner.cpp
    daemon_runner.cpp
)
target_link_libraries(hpactor_demo PRIVATE hpactor_lib)
target_include_directories(hpactor_demo PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/cli_demo
)
