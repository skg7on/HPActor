add_executable(test_system_net
    test_async_io_backend.cpp
    test_event_loop.cpp
    test_connection.cpp
    test_connection_pool.cpp
    test_tls_context.cpp
    test_tls_connection.cpp
    test_tls_integration.cpp
    test_registrar.cpp
    test_registrar_connection.cpp
    test_unix_domain_socket.cpp
    test_uds_integration.cpp
    test_tcp_transport_comprehensive.cpp
    test_reactor_dispatcher.cpp
    test_proactor_dispatcher.cpp
    test_gossip_membership.cpp
    test_service_discovery.cpp
    test_hybrid_discovery.cpp
    test_http_connection.cpp
    test_http_gateway.cpp
)
target_link_libraries(test_system_net hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_system_net PROPERTIES TIMEOUT 30)

# test_iouring_backend depends on liburing (Linux-only, requires ENABLE_PROACTOR=ON)
if(liburing_FOUND)
    target_sources(test_system_net PRIVATE test_iouring_backend.cpp)
endif()
