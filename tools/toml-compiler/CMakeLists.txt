# AOT compiler — compiles TOML topology into mmap-friendly binary format
add_executable(hpactor_toml_compiler
    compiler.cpp
)
target_link_libraries(hpactor_toml_compiler PRIVATE hpactor_lib)

# Enable exceptions for toml++ (linked through hpactor_lib)
set_source_files_properties(
    compiler.cpp
    PROPERTIES COMPILE_FLAGS "-fexceptions"
)
