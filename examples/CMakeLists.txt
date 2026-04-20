# =============================================================================
# HPActor Examples
# =============================================================================

add_executable(01_echo_actor 01_echo_actor.cpp)
target_link_libraries(01_echo_actor PRIVATE hpactor_lib)

add_executable(02_counter_stateful 02_counter_stateful.cpp)
target_link_libraries(02_counter_stateful PRIVATE hpactor_lib)

add_executable(03_typed_calculator 03_typed_calculator.cpp)
target_link_libraries(03_typed_calculator PRIVATE hpactor_lib)

add_executable(04_supervision_tree 04_supervision_tree.cpp)
target_link_libraries(04_supervision_tree PRIVATE hpactor_lib)

add_executable(05_ping_pong 05_ping_pong.cpp)
target_link_libraries(05_ping_pong PRIVATE hpactor_lib)

add_executable(07_priority_scheduler_demo 07_priority_scheduler_demo.cpp)
target_link_libraries(07_priority_scheduler_demo PRIVATE hpactor_lib)

if(HPACTOR_USE_COROUTINES)
add_executable(08_coroutine_scheduler_demo 08_coroutine_scheduler_demo.cpp)
target_link_libraries(08_coroutine_scheduler_demo PRIVATE hpactor_lib)
endif()
