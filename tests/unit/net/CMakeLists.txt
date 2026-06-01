add_executable(test_unit_net
    test_frame.cpp
    test_frame_edge_cases.cpp
    test_communication_endpoint.cpp
    test_connection.cpp
    test_connection_pool.cpp
    test_endpoint_outbound_queue.cpp
    test_gossip_membership.cpp
    test_http_parser.cpp
    test_http_serializer.cpp
    test_hybrid_discovery.cpp
    test_proactor_dispatcher.cpp
    test_registrar.cpp
    test_registrar_connection.cpp
    test_registrar_serialization.cpp
    test_service_discovery.cpp
    test_tls_context.cpp
    test_tls_integration.cpp
)
target_link_libraries(test_unit_net hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_net PROPERTIES TIMEOUT 30)
