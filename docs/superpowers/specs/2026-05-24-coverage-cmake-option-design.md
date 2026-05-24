# ENABLE_COVERAGE CMake Option Design

**Date:** 2026-05-24
**Status:** approved
**Approach:** single `ENABLE_COVERAGE` cmake option with dedicated `cmake/coverage.cmake` module

## Summary

Add an `ENABLE_COVERAGE` cmake option (default OFF) that sets `--coverage`
compiler/linker flags and `-fprofile-update=atomic`, replacing the manual
`-DCMAKE_CXX_FLAGS="--coverage ..."` flags currently passed in CI. Simplify the
CI coverage job by using `-DENABLE_COVERAGE=ON`.

## Motivation

CI already captures branch coverage via `lcov --rc branch_coverage=1` and
`genhtml --branch-coverage`, but the `--coverage` compiler flags are set
manually via `CMAKE_CXX_FLAGS`, `CMAKE_C_FLAGS`, and linker flags in the CI YAML.
There is no cmake-level coverage option for local development or CI reuse.

## Design

### 1. New cmake option

In `CMakeLists.txt`, alongside the existing `ENABLE_*` options:

```cmake
option(ENABLE_COVERAGE "Enable code coverage instrumentation (gcc/clang)" OFF)
```

### 2. New cmake module

`cmake/coverage.cmake` — isolated module, included after `compiler_setup.cmake`:

```cmake
if(ENABLE_COVERAGE)
    message(STATUS "Coverage instrumentation enabled")
    add_compile_options(--coverage -fprofile-update=atomic)
    add_link_options(--coverage)
endif()
```

`-fprofile-update=atomic` is included for thread-safe coverage counter updates
in the multi-threaded scheduler. Already in use in the CI job.

### 3. CI workflow simplification

In `.github/workflows/ci.yml`, the `coverage` job, replace:

```yaml
-DCMAKE_CXX_FLAGS="--coverage -fprofile-update=atomic"
-DCMAKE_C_FLAGS="--coverage -fprofile-update=atomic"
-DCMAKE_EXE_LINKER_FLAGS="--coverage"
-DCMAKE_SHARED_LINKER_FLAGS="--coverage"
```

With:

```yaml
-DENABLE_COVERAGE=ON
```

### Files Changed

| File | Change |
|------|--------|
| `CMakeLists.txt` | Add `option(ENABLE_COVERAGE ...)` |
| `cmake/coverage.cmake` | New: coverage flag module |
| `.github/workflows/ci.yml` | Replace 4 manual flags with `-DENABLE_COVERAGE=ON` |

### Testing

- Verify `cmake -DENABLE_COVERAGE=ON` adds `--coverage` to compile and link commands
- Verify `cmake` (without the option) does not add coverage flags
- Verify `.gcno` files are produced in the build directory with coverage enabled
- CI: coverage job must continue to produce a valid HTML report with branch coverage
