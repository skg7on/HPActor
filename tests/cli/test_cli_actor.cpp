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

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <hpactor/cli/cli_actor.hpp>

// ---------------------------------------------------------------------------
// Tests for CliActor utility functions.
//
// CliActor::get_history_path() is a public static method that resolves the
// CLI history file path from a CliConfig.
//
// parse_actor_id() is a file-scope (static) helper in cli_actor.cpp and is
// not accessible from tests.  If the symbol becomes exported in the future,
// uncomment the forward declaration and the parse_actor_id tests below.
// ---------------------------------------------------------------------------

using namespace hpactor;
using namespace hpactor::cli;

void test_history_path_config() {
    CliConfig cfg;
    cfg.history_path = "/tmp/test_cli_history.txt";
    std::string result = CliActor::get_history_path(cfg);
    assert(result == "/tmp/test_cli_history.txt");
    printf("  PASSED test_history_path_config\n");
}

void test_history_path_home_fallback() {
    CliConfig cfg;
    cfg.history_path = "";
    std::string result = CliActor::get_history_path(cfg);
    const char* home = getenv("HOME");
    if (home) {
        assert(result == std::string(home) + "/.hpactor_history");
    } else {
        assert(result == "/tmp/.hpactor_history");
    }
    printf("  PASSED test_history_path_home_fallback\n");
}

// ---------------------------------------------------------------------------
// parse_actor_id tests — currently file-scoped static in cli_actor.cpp.
// If parse_actor_id is made accessible in the future, uncomment below.
// ---------------------------------------------------------------------------
#if 0
// Forward declaration of the internal helper (not in any public header)
namespace hpactor { namespace cli {
ActorId parse_actor_id(const std::string& s);
}}

void test_parse_actor_id_decimal() {
    ActorId id = parse_actor_id("42");
    assert(id.value() == 42);
    printf("  PASSED test_parse_actor_id_decimal\n");
}

void test_parse_actor_id_hex() {
    ActorId id = parse_actor_id("0xFF");
    assert(id.value() == 255);
    printf("  PASSED test_parse_actor_id_hex\n");
}

void test_parse_actor_id_invalid() {
    ActorId id = parse_actor_id("abc");
    assert(id.value() == 0);
    printf("  PASSED test_parse_actor_id_invalid\n");
}
#endif

int main() {
    printf("CliActor utility tests:\n");
    test_history_path_config();
    test_history_path_home_fallback();
    printf("All CliActor utility tests PASSED\n");
    return 0;
}
