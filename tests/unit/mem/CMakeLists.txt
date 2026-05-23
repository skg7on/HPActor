add_executable(test_unit_mem
    test_size_class.cpp
    test_alloc_header.cpp
    test_freelist.cpp
    test_segment_provider.cpp
    test_slab_cache.cpp
    test_thread_local_allocator.cpp
    test_memory_stress.cpp
    test_memory_tracker.cpp
    test_telemetry_ring_buffer.cpp
    test_memory_poisoning.cpp
    test_hibernation.cpp
    test_guard_page.cpp
    test_compaction.cpp
    test_allocator_benchmark.cpp
    test_std_allocator.cpp
    test_memory_region_accounting.cpp
)
target_link_libraries(test_unit_mem hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_mem PROPERTIES TIMEOUT 60)
