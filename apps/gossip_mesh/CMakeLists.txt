add_executable(20_gossip_mesh
    main.cpp
    scenario.cpp
)
target_link_libraries(20_gossip_mesh PRIVATE
    hpactor_lib
    hpactor_cluster
)
target_include_directories(20_gossip_mesh PRIVATE ${CMAKE_SOURCE_DIR})
