# =============================================================================
# coverage.cmake — code coverage instrumentation (gcc/clang)
# =============================================================================
#
# Included from the top-level CMakeLists.txt (same scope).  When ENABLE_COVERAGE
# is ON, adds --coverage to both compile and link options so .gcno / .gcda files
# are produced.  -fprofile-update=atomic keeps counters correct under the
# multi-threaded work-stealing scheduler.

if(ENABLE_COVERAGE)
    message(STATUS "Coverage instrumentation enabled")
    add_compile_options(--coverage -fprofile-update=atomic)
    add_link_options(--coverage)
    add_compile_definitions(HPACTOR_COVERAGE_BUILD)
endif()
