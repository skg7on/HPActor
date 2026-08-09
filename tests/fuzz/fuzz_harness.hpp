// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

/// \brief Shared helpers for libFuzzer-based fuzz targets.
///
/// Include this header first in every fuzz_*.cpp target.
/// Provides \c fuzz_entry() — a consistent entry wrapper that silences
/// production output during fuzzing.
///
/// HPActor builds with \c -fno-exceptions, so the code under test never
/// throws.  Bugs that would manifest as exceptions (e.g., \c std::stof()
/// in \c parse_accept_header()) surface as \c std::terminate / SIGABRT,
/// which libFuzzer treats as a crash.

namespace hpactor {
namespace fuzz {

/// \brief Redirect stdout/stderr to /dev/null for quiet fuzzing.
///
/// Set \c FUZZ_VERBOSE=1 in the environment to keep output for debugging.
/// Called once per process; subsequent calls are no-ops.
inline void silence_output() {
    static bool silenced = false;
    if (silenced)
        return;
    silenced = true;
    const char* env = std::getenv("FUZZ_VERBOSE");
    if (!env || env[0] == '\0' || env[0] == '0') {
        std::freopen("/dev/null", "w", stdout);
        std::freopen("/dev/null", "w", stderr);
    }
}

} // namespace fuzz
} // namespace hpactor

/// \brief Consistent libFuzzer entry wrapper.
///
/// Silences production output, then invokes the test function.
/// HPActor code does not throw (built with \c -fno-exceptions);
/// bugs that trigger \c std::terminate surface as crashes that
/// libFuzzer records.
///
/// Usage:
/// \code{.cpp}
/// extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
///     return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
///         // ... exercise the code under test with (d, s) ...
///     });
/// }
/// \endcode
///
/// \tparam F A callable accepting \c (const uint8_t*, size_t).
/// \param data  Fuzzer-generated input bytes.
/// \param size  Number of input bytes.
/// \param fn    The test function to invoke.
/// \return 0.
template <typename F> int fuzz_entry(const uint8_t* data, size_t size, F&& fn) {
    hpactor::fuzz::silence_output();
    fn(data, size);
    return 0;
}
