cmake_minimum_required(VERSION 3.16)
project(HPActor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce TSan in CI builds
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# Header-only library
add_library(hpactor INTERFACE)
target_include_directories(hpactor INTERFACE include)

# Tests
enable_testing()
add_subdirectory(tests)