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

/// \file fuzz_cli_lexer.cpp
/// \brief Fuzz target for \c Lexer::tokenize() — the CLI command-line
/// tokenizer.

#include "fuzz_harness.hpp"
#include <hpactor/cli/io/lexer.hpp>
#include <hpactor/cli/token.hpp>

#include <string>

using namespace hpactor;
using namespace hpactor::cli;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        std::string input(reinterpret_cast<const char*>(d), s);

        // Core lexer entry point
        auto tokens = cli::Lexer::tokenize(input);

        // Verify invariants
        if (!tokens.empty()) {
            // Last token must be Eof
            if (tokens.back().type != TokenType::Eof) {
                // Contract violation — this is a bug worth crashing on
                std::abort();
            }
        }

        // Exercise token accessors — touch every token
        for (const auto& tok : tokens) {
            (void)tok.type;
            (void)tok.value.size();
            // Force string copy to exercise allocator paths
            volatile auto copy_len = tok.value.size();
            (void)copy_len;
        }
    });
}
