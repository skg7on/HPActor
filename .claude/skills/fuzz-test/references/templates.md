# Fuzz Test Templates

Copy-paste templates for each file type in a fuzz test addition.
Replace `<name>`, `<subsys>`, and `<EntryPoint>` with real values.

## Fuzz Target (`tests/fuzz/fuzz_<name>.cpp`)

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

/// \file fuzz_<name>.cpp
/// \brief Fuzz target for <EntryPoint>.

#include "fuzz_harness.hpp"
// Add subsystem headers here

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        // Build input from fuzz bytes
        StreamBuffer buf(d, d + s);

        // Test 1: default configuration
        auto r1 = /* call entry point with (buf) */;
        if (r1.ok()) {
            // Exercise the successful result
        }
        (void)r1;

        // Test 2: relaxed/tight limits (if applicable)
        // auto r2 = /* call with different config */;

        // Test 3: incremental feeding (streaming parsers only)
        // if (s > 1) { split at midpoint; feed chunks; }
    });
}
```

### With exceptions (toml++ etc.)

```cpp
/// \note This TU is compiled with -fexceptions (toml++ requirement).

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        // Write to temp file if needed (toml++ requires file path)
        std::string tmpfile = "/tmp/fuzz_<name>_" + std::to_string(std::rand());
        { FILE* f = std::fopen(tmpfile.c_str(), "wb");
          if (!f) return; std::fwrite(d, 1, s, f); std::fclose(f); }

        try {
            auto result = /* call entry point with tmpfile */;
            // Exercise on success
        } catch (const /* expected_error */&) {
            // Expected — malformed input
        }

        std::remove(tmpfile.c_str());
    });
}
```

## Regression Test

### Append to existing test file (preferred)

```cpp
// ── Fuzz regression tests ─────────────────────────────────────────────

TEST(FuzzRegression, <DescriptiveName>) {
    // Replay a known-edge-case input
    uint8_t data[] = { /* bytes */ };
    StreamBuffer buf(data, data + sizeof(data));
    auto r = <EntryPoint>(buf);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, <ExpectedError>);
}
```

### New test file (`tests/unit/<subsys>/test_fuzz_regression_<name>.cpp`)

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

/// \file test_fuzz_regression_<name>.cpp
/// \brief Regression tests for <name> fuzz findings.

#include <gtest/gtest.h>
// Add subsystem headers here

using namespace hpactor;

TEST(FuzzRegression<Name>, EmptyInput) {
    // Must not crash on empty input
}

TEST(FuzzRegression<Name>, ValidRoundTrip) {
    // Construct valid input, encode, decode, compare
}

TEST(FuzzRegression<Name>, MalformedEdgeCase) {
    // Known malformed input → expected error code
}

TEST(FuzzRegression<Name>, AdversarialInput) {
    // Large/deep/nested input → no crash
}
```

## CMake — Fuzz Target Registration

In `tests/fuzz/CMakeLists.txt`:
```cmake
add_fuzz_test(<name>
    SOURCES fuzz_<name>.cpp
    LIBRARIES fuzz_harness hpactor_lib
)
# If the code under test requires exceptions:
target_compile_options(fuzz_<name> PRIVATE -fexceptions)
```

## CMake — Regression Test Registration

In `tests/unit/<subsys>/CMakeLists.txt`, add to the `add_executable` call:
```cmake
add_executable(test_unit_<subsys>
    # ... existing sources ...
    test_fuzz_regression_<name>.cpp
)
```

## Seed Corpus Generation (Python)

```python
import struct, os

base = 'tests/fuzz/fuzz_corpus/<name>'
os.makedirs(base, exist_ok=True)

# Empty
with open(f'{base}/seed_empty.bin', 'wb') as f: f.write(b'')

# Minimal valid
with open(f'{base}/seed_valid_minimal.bin', 'wb') as f:
    f.write(b'<minimal valid bytes>')

# Truncated (partial header)
with open(f'{base}/seed_truncated.bin', 'wb') as f:
    f.write(b'<partial bytes>')

# Max-value edge case
with open(f'{base}/seed_max_edge.bin', 'wb') as f:
    f.write(b'<max-value bytes>')

# Binary garbage
with open(f'{base}/seed_binary.bin', 'wb') as f:
    f.write(bytes(range(256)))

print('Seeds created')
```

## CMake Module Reference

The `cmake/FuzzTest.cmake` module provides:

```cmake
add_fuzz_test(name
    SOURCES fuzz_target.cpp     # Source file(s)
    LIBRARIES fuzz_harness ...  # Libraries to link
)
```

This creates an executable `fuzz_<name>` with:
- `-fsanitize=fuzzer,undefined` (no ASAN by default)
- `EXCLUDE_FROM_ALL` — not in default build
- Not registered with CTest

To opt into ASAN: `cmake -DFUZZ_WITH_ASAN=ON ...`
