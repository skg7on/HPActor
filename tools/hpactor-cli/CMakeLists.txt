# Standalone CLI client — connects to hpactor daemon via UDS or TCP
add_executable(hpactor-cli main.cpp)
target_link_libraries(hpactor-cli PRIVATE hpactor_lib)
target_include_directories(hpactor-cli PRIVATE ${CMAKE_SOURCE_DIR}/include)
