---
name: fuzz-test
description: >
  Use this skill when the user mentions fuzzing, a fuzz target, libFuzzer, or
  coverage-guided fuzzing — whether writing a fuzz harness, adding fuzz tests for
  a parser or decoder, or chasing a crash, memory error, or invariant violation
  from malformed input. Applies to code consuming untrusted or malformed bytes:
  parsers, decoders, protocol handlers, deserializers, network frames, wire
  formats, HTTP requests, JSON/protobuf bodies, config files, CLI stdin. Covers
  the full lifecycle — writing the target and seed corpus, regression tests,
  CMake wiring, running the fuzzer, triaging findings — and getting-started
  questions. Skip ordinary unit/integration tests, stress/chaos/soak tests, and
  benchmarks.
---

# Fuzz Test Development

Coverage-guided fuzz testing for HPActor input-facing parsers and decoders
using libFuzzer. This skill covers the full lifecycle: identifying the attack
surface, writing the fuzz target, creating seed corpora, adding regression
tests, wiring CMake, and verifying the build.

## Quick Reference

Reference files in `references/`:
- `references/templates.md` — copy-paste templates for every file type

Load the templates reference when you need the exact code shapes; the
sections below explain the decisions and workflow.

## Workflow

### Step 1: Identify the Fuzz Surface

For each input-facing subsystem, answer:

- **What bytes enter the system?** Network frames, config files, HTTP
  requests, CLI input strings, protobuf payloads, JSON bodies.
- **What is the single entry-point function?** Fuzz targets MUST exercise
  exactly one parse/decode function per target. If a component has multiple
  independent entry points, write separate targets.
- **What are the failure modes?** Buffer underflow (< min header size),
  integer overflow (length fields), unbounded memory growth (repeated
  fields, no max-length enforcement), magic-byte mismatch, trailing bytes,
  invalid UTF-8, deeply nested structures.
- **Does the code use exceptions?** HPActor builds with `-fno-exceptions`
  except for `toml_parser.cpp`, `toml_table_view.cpp`, and
  `toml-compiler/compiler.cpp`. If the code under test requires exceptions
  (e.g., toml++), the fuzz target TU must be compiled with `-fexceptions`
  (see CMake section below).

For each candidate, classify risk:
| Risk | Examples |
|------|----------|
| **Critical** | Network ingress: frame decoder, protobuf parse |
| **High** | HTTP parser, TOML config, file parsing |
| **Medium** | CLI input, admin API dispatch, Accept header parser |

Start with critical targets and work down.

### Step 2: Write the Fuzz Target

Create `tests/fuzz/fuzz_<name>.cpp`. Every target follows this shape:

```cpp
#include "fuzz_harness.hpp"
// ... subsystem headers ...

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        // Exercise the entry point with (d, s)
        // ...
    });
}
```

**Rules for the lambda body:**

1. **Touch every error path.** Call the entry point with multiple
   configurations (default limits, relaxed limits, tight limits) so the
   fuzzer explores all branches.
2. **Round-trip when possible.** If the component has both encode and
   decode, do encode→decode→compare to find consistency bugs.
3. **Exercise incremental paths.** For streaming parsers (HTTP), split
   input at midpoints and feed byte-at-a-time to exercise state machine
   edge cases.
4. **Never call `exit()` or `abort()` except for detected invariant
   violations.** Let the fuzzer discover crashes via sanitizers.
5. **Guard expensive operations behind size checks.** Byte-at-a-time
   feeding should only happen for inputs ≤ 256 bytes.
6. **Touch nested sub-objects.** After a successful protobuf parse, access
   sub-messages and repeated fields to exercise nested decode paths.
7. **Use `(void)` to suppress unused-variable warnings.** The fuzzer
   cares about code coverage, not return values.

### Step 3: Create Seed Corpus

Create `tests/fuzz/fuzz_corpus/<name>/` with binary seed files. Seeds jump-start
the fuzzer by providing valid and edge-case inputs.

**Minimum seed set (5-8 files):**
- Empty input (zero bytes)
- Minimal valid input (e.g., "HPAC" + 0-length for frames)
- Valid input with typical content
- Truncated input (missing header, partial payload)
- Max-value edge case (e.g., 0xFFFFFFFF length field)
- Wrong magic/signature bytes
- Binary garbage (0x00–0xFF)

Use Python for binary seeds:
```bash
python3 -c "
import struct
with open('seed_valid.bin', 'wb') as f:
    f.write(b'HPAC' + struct.pack('>I', 0))
"
```

### Step 4: Add Regression Tests

For every fuzz target, add focused regression tests that replay known
edge-case inputs and assert the expected outcome (success or specific error
code). These catch regressions when the code under test changes.

Place regression tests in the existing subsystem test file when possible
(e.g., `tests/unit/net/test_frame.cpp`). Create a new file
`tests/unit/<subsys>/test_fuzz_regression_<name>.cpp` only when no suitable
existing file exists.

**Each regression test MUST:**
- Exercise production code (hpactor:: namespace) — never be a pure
  language-level assertion
- Assert either success (valid input → ok) or a specific error code
  (malformed input → FrameDecodeError::HeaderTooShort etc.)
- Never crash regardless of input validity

**Template:**
```cpp
TEST(FuzzRegression, FrameEmptyInput) {
    StreamBuffer buf;
    auto r = try_decode_wireframe(buf);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, FrameDecodeError::HeaderTooShort);
}
```

### Step 5: Wire CMake

**5a. Register the fuzz target** in `tests/fuzz/CMakeLists.txt`:
```cmake
add_fuzz_test(<name>
    SOURCES fuzz_<name>.cpp
    LIBRARIES fuzz_harness hpactor_lib
)
```

**5b. If the code under test requires exceptions** (toml++, etc.):
```cmake
target_compile_options(fuzz_<name> PRIVATE -fexceptions)
```

**5c. Register the regression test** by adding the .cpp file to the
existing `tests/unit/<subsys>/CMakeLists.txt` `add_executable` call.
Regression tests are regular GTest cases — no special flags needed.

### Step 6: Verify

**Step 6a. Regression tests first (no fuzz toolchain needed):**
```bash
ninja -C build test_unit_<subsys>
./build/tests/unit/<subsys>/test_unit_<subsys> --gtest_filter="FuzzRegression*"
```

**Step 6b. Build and smoke-test fuzz targets:**
```bash
cmake -S . -B build_fuzz -GNinja -DENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
ninja -C build_fuzz fuzz_<name>
./build_fuzz/tests/fuzz/fuzz_<name> -max_total_time=3 -runs=10000
```

**Step 6c. Verify no regressions in existing tests:**
```bash
ctest --test-dir build -R "FuzzRegression|<ExistingTestNames>" --output-on-failure
```

**Step 6d. Deep fuzz (offline, not per-commit):**
```bash
./build_fuzz/tests/fuzz/fuzz_<name> -max_total_time=3600 -jobs=4 -workers=4
```

### Step 7: Triage Crashes

When libFuzzer finds a crash:

1. **Minimize:** `./build_fuzz/tests/fuzz/fuzz_<name> crash-<hash> -minimize_crash=1`
2. **Reproduce under lldb:** `lldb -- ./build_fuzz/tests/fuzz/fuzz_<name> minimized_crash`
3. **Write a regression test** that replays the minimized input
4. **File a bug** with the minimized input attached
5. **Fix the bug** — then the regression test turns from RED to GREEN

## Design Constraints

- Fuzz targets are NOT registered with CTest — they run offline
- Fuzz targets are EXCLUDE_FROM_ALL — build explicitly by name
- `ENABLE_FUZZ` CMake option defaults to OFF
- Fuzz targets require Clang (libFuzzer is Clang-only)
- On macOS ARM, omit ASAN from sanitizer flags by default (known
  `asan_init_is_running` CHECK failure with libFuzzer)
- Use `-fsanitize=fuzzer,undefined` as the default sanitizer set;
  pass `-DFUZZ_WITH_ASAN=ON` to add address sanitizer
- Fuzz targets link `hpactor_lib` directly, NOT GTest
- HPActor's `-fno-exceptions` is inherited from hpactor_lib; fuzz targets
  that need exceptions must override with `-fexceptions`

## What NOT to Fuzz

This skill is for input-parsing fuzz targets. Do NOT use it for:
- **Live ActorSystem fuzzing** — requires full scheduler/network setup;
  covered by chaos tests (TST-002)
- **Protobuf library internals** — upstream responsibility; fuzz HPActor's
  *usage* of protobuf (oneof dispatch, field access patterns)
- **llhttp internals** — upstream responsibility; fuzz HPActor's
  *wrapper* (HttpParser state machine, unbounded accumulation,
  HttpSerializer call chain)
- **General unit/integration tests** — use the existing GTest framework
