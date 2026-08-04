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

/// \file fuzz_toml.cpp
/// \brief Fuzz target for the TOML config parser (toml++ library).
///
/// Writes fuzz bytes to a temp file and feeds it to \c toml::parse_file().
/// Exercises the full TOML v1.0.0 grammar without mocking the filesystem.
///
/// \note This TU is compiled with \c -fexceptions because toml++ requires
///       exception support (same exemption as \c toml_parser.cpp).

#include "fuzz_harness.hpp"

#include <toml.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        // Write fuzz input to temp file (toml++ requires a file path)
        std::string tmpfile = "/tmp/fuzz_toml_" + std::to_string(std::rand());
        {
            FILE* f = std::fopen(tmpfile.c_str(), "wb");
            if (!f)
                return;
            std::fwrite(d, 1, s, f);
            std::fclose(f);
        }

        // Exercise toml++ parser — core attack surface.
        // toml::parse_file() throws toml::parse_error on malformed input.
        // This TU is compiled with -fexceptions (toml++ requirement).
        try {
            auto tbl = toml::parse_file(tmpfile);
            // On success, exercise the table traversal to cover more paths
            for (const auto& [key, node] : tbl) {
                (void)key.str().size();
                (void)node.is_table();
                (void)node.is_array();
                (void)node.is_string();
                (void)node.is_integer();
                (void)node.is_floating_point();
                (void)node.is_boolean();
                (void)node.is_date();
                (void)node.is_time();
            }
        } catch (const toml::parse_error&) {
            // Expected — malformed TOML input
        } catch (const std::bad_alloc&) {
            // Memory exhaustion — also a valid finding for the fuzzer
        }

        std::remove(tmpfile.c_str());
    });
}
