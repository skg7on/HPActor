# =============================================================================
# HPActor Example 18: Cluster Control Plane Demo
# =============================================================================
# Demonstrates building a distributed control plane with the HPActor Cluster
# subsystem: failure model (CLU-001), sharding (CLU-002), singleton (CLU-003),
# reliable messaging (MSG-003), and durable state (DUR-001/002).
# =============================================================================

add_executable(18_cluster_control_plane
    18_cluster_control_plane.cpp
    scenario.cpp
)
target_link_libraries(18_cluster_control_plane PRIVATE hpactor_lib hpactor_cluster)
target_include_directories(18_cluster_control_plane PRIVATE ${CMAKE_SOURCE_DIR})
