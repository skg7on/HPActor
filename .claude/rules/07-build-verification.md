# 7. Build & Verification

## Configure & Build

```
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```

Do not reconfigure CMake unless CMake files, options, generated protobuf
inputs, or toolchain assumptions changed.

## Verification Strategy

**Prefer the narrowest verification that covers the changed surface:**

1. A targeted `ninja` target that compiles only the changed TU.
2. One test binary (`./build/tests/unit/core/test_unit_core`).
3. A CTest pattern (`ctest -R <pattern> --output-on-failure`).

**Only run a full configure/build/test cycle when** the change affects build
configuration, generated files, broad public headers, cross-cutting runtime
behavior, or when the user explicitly asks for it.

## Common Test Commands

```bash
# List available GTest cases in a binary
./build/tests/unit/core/test_unit_core --gtest_list_tests

# Run a specific GTest case
./build/tests/unit/core/test_unit_core --gtest_filter="*ActorId*"

# Run via ctest with pattern matching
ctest -R "ActorIdDefaultConstruction" --output-on-failure

# Full parallel test suite
ctest --output-on-failure --parallel 8
```
