# =============================================================================
# src/msg/CMakeLists.txt — Message serialization subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    delivery_receipt.cpp
    frame.cpp
    frame_protobuf.cpp
    outbound_delivery_tracker.cpp
    proto_type_registry.cpp
)
