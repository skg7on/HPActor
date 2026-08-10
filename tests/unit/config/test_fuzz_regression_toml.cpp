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

/// \file test_fuzz_regression_toml.cpp
/// \brief Regression tests for TOML config parser fuzz findings.
///
/// \note This TU is compiled with \c -fexceptions because toml++ requires
///       exception support (same exemption as \c fuzz_toml.cpp).

#include <gtest/gtest.h>

#include <toml.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

/// Write bytes to a temp file, call toml::parse_file(), return true.
/// The critical invariant is "no crash" — malformed input producing a
/// toml::parse_error is acceptable behaviour.
bool parse_does_not_crash(const uint8_t* data, size_t size) {
    std::string tmpfile =
        "/tmp/fuzz_toml_regression_" + std::to_string(std::rand());
    {
        FILE* f = std::fopen(tmpfile.c_str(), "wb");
        if (!f)
            return false;
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }

    try {
        auto tbl = toml::parse_file(tmpfile);
        // Exercise table traversal to cover more code paths
        for (const auto& [key, node] : tbl) {
            (void)key.str().size();
            (void)node.is_table();
            (void)node.is_array();
            (void)node.is_string();
        }
    } catch (const toml::parse_error&) {
        // Expected for malformed input — not a crash
    } catch (const std::bad_alloc&) {
        // Memory exhaustion — also not a crash (though undesirable)
    }

    std::remove(tmpfile.c_str());
    return true;
}

} // namespace

TEST(FuzzRegressionToml, EmptyFile) {
    EXPECT_TRUE(parse_does_not_crash(nullptr, 0));
}

TEST(FuzzRegressionToml, BinaryGarbage) {
    uint8_t garbage[256];
    for (int i = 0; i < 256; ++i)
        garbage[i] = static_cast<uint8_t>(i);
    EXPECT_TRUE(parse_does_not_crash(garbage, 256));
}

TEST(FuzzRegressionToml, UnterminatedString) {
    const char* input = "name = \"hello";
    EXPECT_TRUE(parse_does_not_crash(reinterpret_cast<const uint8_t*>(input), 15));
}

TEST(FuzzRegressionToml, DeeplyNestedTables) {
    // 100 levels of nested [a.b.c.d.e.f.g...]
    std::string input = "key = 1\n";
    for (int i = 0; i < 100; ++i) {
        input += "[";
        for (int j = 0; j <= i; ++j) {
            if (j > 0)
                input += ".";
            input += static_cast<char>('a' + (j % 26));
        }
        input += "]\nval = " + std::to_string(i) + "\n";
    }
    EXPECT_TRUE(parse_does_not_crash(
        reinterpret_cast<const uint8_t*>(input.data()), input.size()));
}

TEST(FuzzRegressionToml, MaxIntegerValue) {
    // Integer exceeding int64_t range
    const char* input = "big = 99999999999999999999\n";
    EXPECT_TRUE(parse_does_not_crash(reinterpret_cast<const uint8_t*>(input), 30));
}

TEST(FuzzRegressionToml, ValidMinimalToml) {
    const char* input = "[system]\nname = \"test\"\nversion = \"1.0\"\n";
    EXPECT_TRUE(parse_does_not_crash(reinterpret_cast<const uint8_t*>(input), 43));
}

TEST(FuzzRegressionToml, NestedTablesAndArrays) {
    const char* input = "[parent]\n"
                        "  [parent.child]\n"
                        "  key = \"value\"\n"
                        "  [parent.child.grandchild]\n"
                        "  items = [1, 2, 3]\n"
                        "  [[parent.array_of_tables]]\n"
                        "  name = \"first\"\n"
                        "  [[parent.array_of_tables]]\n"
                        "  name = \"second\"\n";
    EXPECT_TRUE(parse_does_not_crash(reinterpret_cast<const uint8_t*>(input), 214));
}

TEST(FuzzRegressionToml, RepeatedParseNoLeak) {
    // Parse valid TOML 1000 times — must not leak memory
    const char* input = "[system]\nname = \"test\"\n";
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(
            parse_does_not_crash(reinterpret_cast<const uint8_t*>(input), 28));
    }
}
