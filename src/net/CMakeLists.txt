# =============================================================================
# src/net/CMakeLists.txt — Network subsystem sources
# =============================================================================

target_sources(hpactor_lib PRIVATE
    acceptor.cpp
    actor_location_cache.cpp
    connection.cpp
    connection_pool.cpp
    endpoint.cpp
    endpoint_circuit_breaker.cpp
    endpoint_outbound_queue.cpp
    event_loop.cpp
    gossip_membership.cpp
    http_client.cpp
    http_connection.cpp
    http_gateway.cpp
    http_parser.cpp
    hybrid_discovery.cpp
    inbound_frame_router.cpp
    registrar.cpp
    registrar_client.cpp
    registrar_server.cpp
    reliable_ack.cpp
    static_discovery.cpp
    tcp_transport.cpp
    tls_connection.cpp
    tls_context.cpp
    udp_transport.cpp
    wireframe_connection.cpp
)

# ---- platform-specific I/O backends ----------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(HPACTOR_ENABLE_PROACTOR)
        message(STATUS "Using io_uring (Proactor)")
        target_sources(hpactor_lib PRIVATE proactor/iouring_backend.cpp)
    else()
        message(STATUS "Using epoll (Reactor)")
    endif()
    target_sources(hpactor_lib PRIVATE reactor/epoll_backend.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(HPACTOR_ENABLE_PROACTOR)
        message(STATUS "Using GCD (Proactor)")
        target_sources(hpactor_lib PRIVATE proactor/gcd_backend.cpp)
    else()
        message(STATUS "Using kqueue (Reactor)")
    endif()
    target_sources(hpactor_lib PRIVATE reactor/kqueue_backend.cpp)
endif()
