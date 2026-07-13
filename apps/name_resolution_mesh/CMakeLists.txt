add_executable(19_name_resolution_mesh
    main.cpp
    scenario.cpp
)
target_link_libraries(19_name_resolution_mesh PRIVATE
    hpactor_lib
    hpactor_cluster
)
target_include_directories(19_name_resolution_mesh PRIVATE ${CMAKE_SOURCE_DIR})
