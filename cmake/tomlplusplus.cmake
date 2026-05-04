# toml++ — C++20 header-only TOML parser (vendored single-header v3.4.0)
# Header stored in third_party/tomlplusplus/toml.hpp

add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus)
target_compile_features(tomlplusplus INTERFACE cxx_std_20)
add_library(tomlplusplus::tomlplusplus ALIAS tomlplusplus)
