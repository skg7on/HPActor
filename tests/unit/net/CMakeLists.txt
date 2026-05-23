add_executable(test_unit_net
    test_frame.cpp
    test_communication_endpoint.cpp
    test_http_parser.cpp
    test_http_serializer.cpp
    test_registrar_serialization.cpp
)
target_link_libraries(test_unit_net hpactor hpactor_proto GTest::gtest_main)
gtest_discover_tests(test_unit_net)
