add_executable(test_integration_net
    test_event_loop.cpp
    test_tcp_transport_comprehensive.cpp
    test_registrar_framing.cpp
    test_udp_registrar_packets.cpp
    test_http_gateway.cpp
    test_reactor_dispatcher.cpp
    test_tls_connection.cpp
    test_unix_domain_socket.cpp
    test_uds_integration.cpp
    test_http_connection.cpp
    test_connection_pool_outbound.cpp
    test_net_edge_cases.cpp
    test_net_branches.cpp
    test_transport_branches.cpp
    test_tls_branches.cpp
)
target_link_libraries(test_integration_net hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_net PROPERTIES TIMEOUT 30)

# test_iouring_backend depends on liburing (Linux-only, requires ENABLE_PROACTOR=ON)
if(liburing_FOUND)
    target_sources(test_integration_net PRIVATE test_iouring_backend.cpp)
endif()
