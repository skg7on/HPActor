# Actor System CLI Interactive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an interactive CLI subsystem for HPActor that provides actor introspection, system management, and monitoring via a hierarchical command tree with thread-safe Inspect Message request-response pattern.

**Architecture:** A `CliActor` (dedicated-thread `DaemonActor`) blocks on stdin/UDS for input via its `run_once()` loop, tokenizes via a whitespace lexer, and traverses a trie-based `CommandNode` tree to dispatch commands. Commands that inspect actors send `InspectStateRequest` system messages — the target actor handles the request on its own thread and replies with `InspectStateReply` containing metadata from `to_metadata()` and `serialize_state()`. Output is rendered by pluggable `OutputFormatter` implementations (Pretty, JSON, Tabular). A `Pager` state machine provides interactive paging for `/actor list` with cursor-based registry enumeration.

**Tech Stack:** C++20, `std::function`, protobuf for CLI messages, `DaemonActor` for dedicated I/O thread, `std::from_chars` for no-exception parsing, UNIX domain sockets for remote attach, CMake with Ninja.

**Spec:** `docs/superpowers/specs/2026-05-05-actor-cli-interactive-design.md`
**Architecture doc:** `docs/architecture/actor/cli-interactive-design.md`

---

## File Structure

| File | Purpose |
|------|---------|
| `include/hpactor/cli/cli_config.hpp` | `CliConfig` struct (created in Task 1) |
| `include/hpactor/cli/cli_types.hpp` | `ActorMetadata`, `MailboxSnapshot`, `ChildInfo` structs (no protobuf dependency) |
| `include/hpactor/cli/token.hpp` | `Token`, `TokenType` enum |
| `include/hpactor/cli/lexer.hpp` | `Lexer` class — string → token stream |
| `include/hpactor/cli/command_node.hpp` | `CommandNode` trie data structure |
| `include/hpactor/cli/command_context.hpp` | `CommandContext` — args, params, system ptr |
| `include/hpactor/cli/output_formatter.hpp` | `OutputFormatter` abstract interface |
| `include/hpactor/cli/pretty_formatter.hpp` | `PrettyFormatter` — ANSI box-drawing |
| `include/hpactor/cli/json_formatter.hpp` | `JsonFormatter` — machine-readable |
| `include/hpactor/cli/tabular_formatter.hpp` | `TabularFormatter` — whitespace-aligned |
| `include/hpactor/cli/pager.hpp` | `Pager` — interactive paging state machine |
| `include/hpactor/cli/cli_actor.hpp` | `CliActor` — DaemonActor subclass |
| `src/cli/lexer.cpp` | Lexer implementation |
| `src/cli/command_node.cpp` | CommandNode tree building + traversal |
| `src/cli/pretty_formatter.cpp` | PrettyFormatter implementation |
| `src/cli/json_formatter.cpp` | JsonFormatter implementation |
| `src/cli/tabular_formatter.cpp` | TabularFormatter implementation |
| `src/cli/pager.cpp` | Pager implementation |
| `src/cli/cli_actor.cpp` | CliActor implementation |
| `include/hpactor/cli/commands/show_command.hpp` | `/actor <id> show` |
| `include/hpactor/cli/commands/kill_command.hpp` | `/actor <id> kill` |
| `include/hpactor/cli/commands/list_command.hpp` | `/actor list` |
| `include/hpactor/cli/commands/system_stats_command.hpp` | `/system stats` |
| `include/hpactor/cli/commands/system_memory_command.hpp` | `/system memory` |
| `protos/hpactor/cli_messages.proto` | InspectState, Kill, List, Stats, Memory messages |
| `include/hpactor/actor/abstract_actor.hpp` | **Modified** — add `to_metadata()`, `serialize_state()`, `mailbox_snapshot()` |
| `include/hpactor/core/actor_system.hpp` | **Modified** — add `CliConfig`, `cli_actor_` |
| `src/actor/actor_system.cpp` | **Modified** — spawn CliActor, TOML config |
| `src/actor/event_based_actor.cpp` | **Modified** — system dispatch for CLI request tags |
| `include/hpactor/types/types.hpp` | **Modified** — add CLI TypeTags (15-28) |
| `CMakeLists.txt` | **Modified** — add cli sources, tests, `ENABLE_CLI` option |

---

### Task 1: Create CLI TypeTags and Protobuf Messages

**Files:**
- Modify: `include/hpactor/types/types.hpp`
- Create: `protos/hpactor/cli_messages.proto`

- [ ] **Step 1: Add CLI TypeTags to the TypeTag enum**

Read `include/hpactor/types/types.hpp`. Find the `TypeTag` enum (around line 456). After the Metrics subsystem range (`0x40 – 0x4F`, ending at `MetricsResponseTag = 0x41`), add the CLI range (`0x50 – 0x5F`):

```cpp
// CLI interactive subsystem (0x50 – 0x5F)
InspectStateRequestTag   = 0x50,
InspectStateResponseTag  = 0x51,
KillRequestTag           = 0x52,
KillResponseTag          = 0x53,
ListActorsRequestTag     = 0x54,
ListActorsResponseTag    = 0x55,
SystemStatsRequestTag    = 0x56,
SystemStatsResponseTag   = 0x57,
MemoryStatsRequestTag    = 0x58,
MemoryStatsResponseTag   = 0x59,
TopologyShowRequestTag   = 0x5A,
TopologyShowResponseTag  = 0x5B,
TopologyRestartRequestTag = 0x5C,
TopologyRestartResponseTag = 0x5D,
```

Also update the comment block at lines 440-454 to document the `0x50 – 0x5F` sub-range as "CLI interactive".

- [ ] **Step 1b: Create CliConfig and CliTypes headers**

Create `include/hpactor/cli/cli_config.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

struct CliConfig {
    bool enabled = true;
    std::string listen_path;          // UDS path; empty = stdin/stdout
    uint16_t tcp_port = 0;           // TCP port; 0 = disabled
    std::string default_format = "pretty";
    uint32_t page_size = 50;
};

}  // namespace cli
}  // namespace hpactor
```

Create `include/hpactor/cli/cli_types.hpp` (lightweight structs, no protobuf dependency):

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

struct ActorMetadata {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
    uint64_t incarnation = 0;
    uint64_t messages_processed = 0;
    uint64_t uptime_ms = 0;
    std::string behavior_name;
};

struct MailboxSnapshot {
    uint32_t depth = 0;
    uint64_t total_enqueued = 0;
    uint64_t total_dequeued = 0;
    uint64_t max_depth = 0;
    uint32_t high_priority_depth = 0;
};

struct ChildInfo {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 2: Create the CLI protobuf schema**

Create `protos/hpactor/cli_messages.proto`:

```protobuf
syntax = "proto3";
package hpactor.cli;

// ── Actor Inspection ──

message InspectStateRequest {
    uint64 target_actor_id = 1;
    bool include_state = 3;
    bool include_mailbox = 4;
    bool include_children = 5;
}

message InspectStateReply {
    uint64 actor_id = 1;
    ActorMetadata metadata = 2;
    bytes state_blob = 3;
    MailboxSnapshot mailbox = 4;
    repeated ChildInfo children = 5;
}

message ActorMetadata {
    uint64 actor_id = 1;
    string actor_type = 2;
    string state = 3;
    uint64 incarnation = 4;
    uint64 messages_processed = 5;
    uint64 uptime_ms = 6;
    string behavior_name = 7;
}

message MailboxSnapshot {
    uint32 depth = 1;
    uint64 total_enqueued = 2;
    uint64 total_dequeued = 3;
    uint64 max_depth = 4;
    uint32 high_priority_depth = 5;
}

message ChildInfo {
    uint64 actor_id = 1;
    string actor_type = 2;
    string state = 3;
}

// ── Actor Termination ──

message KillRequest {
    uint64 target_actor_id = 1;
    bool force = 2;
}

message KillReply {
    bool success = 1;
    uint32 error_code = 2;
    string error_message = 3;
}

// ── Actor Listing ──

message ListActorsRequest {
    uint32 shard_index = 1;
    uint32 offset = 2;
    uint32 limit = 3;
    string filter = 4;
}

message ListActorsReply {
    repeated ActorMetadata actors = 1;
    bool has_more = 2;
    uint32 next_shard_index = 3;
    uint32 next_offset = 4;
    uint32 total_count = 5;
}

// ── System Stats ──

message SystemStatsRequest {}

message SystemStatsReply {
    uint64 total_actors = 1;
    uint64 running_actors = 2;
    uint64 idle_actors = 3;
    uint32 worker_count = 6;
    double scheduler_utilization = 7;
    uint64 memory_active_bytes = 8;
    uint64 uptime_ms = 10;
}

// ── System Memory ──

message MemoryStatsRequest {
    optional uint64 actor_id = 1;
}

message MemoryStatsReply {
    uint64 active_bytes = 1;
    uint64 peak_bytes = 2;
    uint32 segment_count = 3;
    double slab_hit_rate = 4;
}
```

- [ ] **Step 3: Regenerate protobuf code**

Run: `ninja -C build`

Expected: protobuf generates `cli_messages.pb.h` and `cli_messages.pb.cc`. Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/types/types.hpp protos/hpactor/cli_messages.proto include/hpactor/cli/cli_config.hpp include/hpactor/cli/cli_types.hpp
git commit -m "feat: add CLI TypeTags (0x50-0x5F), cli_messages.proto, CliConfig, and CliTypes"
```

---

### Task 2: Implement Lexer — String → Token Stream

**Files:**
- Create: `include/hpactor/cli/token.hpp`
- Create: `include/hpactor/cli/lexer.hpp`
- Create: `src/cli/lexer.cpp`
- Create: `tests/cli/test_lexer.cpp`

- [ ] **Step 1: Define Token types**

Create `include/hpactor/cli/token.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <optional>
#include <string>

namespace hpactor {
namespace cli {

enum class TokenType {
    Keyword,         // actor, show, list, etc.
    Parameter,       // 0x123, echo-actor-1, "quoted string"
    Flag,            // --detail, --no-pager
    FlagWithArg,     // --format json, --filter Worker
    Eof,
};

struct Token {
    TokenType type = TokenType::Eof;
    std::string value;
    std::optional<std::string> arg;  // populated for FlagWithArg
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 2: Write the lexer header**

Create `include/hpactor/cli/lexer.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/token.hpp>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

class Lexer {
public:
    // Tokenize a command string into a sequence of tokens.
    // A leading "/" becomes a Keyword "/" (optional, parser auto-inserts if missing).
    static std::vector<Token> tokenize(const std::string& input);

private:
    static std::string unescape(const std::string& s);
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 3: Write the lexer implementation**

Create `src/cli/lexer.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/lexer.hpp>
#include <cctype>

namespace hpactor {
namespace cli {

std::string Lexer::unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool escape = false;
    for (char c : s) {
        if (escape) {
            switch (c) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case '\\': out += '\\'; break;
            case '"':  out += '"';  break;
            default:   out += '\\'; out += c; break;
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else {
            out += c;
        }
    }
    return out;
}

std::vector<Token> Lexer::tokenize(const std::string& input) {
    std::vector<Token> tokens;
    size_t i = 0;
    size_t n = input.size();

    while (i < n) {
        // Skip whitespace
        while (i < n && std::isspace(static_cast<unsigned char>(input[i]))) ++i;
        if (i >= n) break;

        Token tok;

        // Leading slash is a keyword "/"
        if (input[i] == '/' && (tokens.empty() || tokens.back().type == TokenType::Eof)) {
            tok.type = TokenType::Keyword;
            tok.value = "/";
            ++i;
            tokens.push_back(std::move(tok));
            continue;
        }

        // Flag (--flag or --flag value)
        if (input[i] == '-' && i + 1 < n && input[i + 1] == '-') {
            i += 2;  // skip --
            size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
            std::string flag_name = input.substr(start, i - start);

            // Peek ahead: if the next token is not a flag, it's a flag argument
            size_t j = i;
            while (j < n && std::isspace(static_cast<unsigned char>(input[j]))) ++j;
            if (j < n && !(input[j] == '-' && j + 1 < n && input[j + 1] == '-')) {
                // Next token is the argument
                i = j;  // skip whitespace
                size_t arg_start = i;
                if (input[i] == '"') {
                    ++i;  // skip opening quote
                    arg_start = i;
                    while (i < n && input[i] != '"') {
                        if (input[i] == '\\') ++i;
                        ++i;
                    }
                    std::string arg = unescape(input.substr(arg_start, i - arg_start));
                    ++i;  // skip closing quote
                    tok.type = TokenType::FlagWithArg;
                    tok.value = std::move(flag_name);
                    tok.arg = std::move(arg);
                } else {
                    while (i < n && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
                    tok.type = TokenType::FlagWithArg;
                    tok.value = std::move(flag_name);
                    tok.arg = input.substr(arg_start, i - arg_start);
                }
            } else {
                tok.type = TokenType::Flag;
                tok.value = std::move(flag_name);
            }
            tokens.push_back(std::move(tok));
            continue;
        }

        // Quoted parameter
        if (input[i] == '"') {
            ++i;  // skip opening quote
            size_t start = i;
            while (i < n && input[i] != '"') {
                if (input[i] == '\\') ++i;
                ++i;
            }
            tok.type = TokenType::Parameter;
            tok.value = unescape(input.substr(start, i - start));
            ++i;  // skip closing quote
            tokens.push_back(std::move(tok));
            continue;
        }

        // Regular keyword or parameter
        {
            size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
            tok.type = TokenType::Keyword;
            tok.value = input.substr(start, i - start);
            tokens.push_back(std::move(tok));
        }
    }

    tokens.push_back(Token{TokenType::Eof, ""});
    return tokens;
}

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 4: Write the lexer unit test**

Create `tests/cli/test_lexer.cpp`:

```cpp
#include <hpactor/cli/lexer.hpp>
#include <cassert>
#include <cstdio>
#include <string>

using namespace hpactor::cli;

void test_simple_keywords() {
    auto tokens = Lexer::tokenize("actor list");
    assert(tokens.size() == 3);  // keyword, keyword, eof
    assert(tokens[0].type == TokenType::Keyword);
    assert(tokens[0].value == "actor");
    assert(tokens[1].type == TokenType::Keyword);
    assert(tokens[1].value == "list");
    assert(tokens[2].type == TokenType::Eof);
}

void test_leading_slash() {
    auto tokens = Lexer::tokenize("/actor 5 show");
    assert(tokens.size() == 4);
    assert(tokens[0].type == TokenType::Keyword);
    assert(tokens[0].value == "/");
    assert(tokens[1].type == TokenType::Keyword);
    assert(tokens[1].value == "actor");
    assert(tokens[2].type == TokenType::Keyword);
    assert(tokens[2].value == "5");
    assert(tokens[3].type == TokenType::Keyword);
    assert(tokens[3].value == "show");
}

void test_flags() {
    auto tokens = Lexer::tokenize("/actor list --detail --no-pager");
    assert(tokens.size() >= 4);
    // Find the flags
    bool found_detail = false, found_nopager = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Flag && t.value == "detail") found_detail = true;
        if (t.type == TokenType::Flag && t.value == "no-pager") found_nopager = true;
    }
    assert(found_detail);
    assert(found_nopager);
}

void test_flag_with_arg() {
    auto tokens = Lexer::tokenize("/actor list --format json --filter Worker");
    bool found_format = false, found_filter = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::FlagWithArg && t.value == "format") {
            found_format = true;
            assert(t.arg && *t.arg == "json");
        }
        if (t.type == TokenType::FlagWithArg && t.value == "filter") {
            found_filter = true;
            assert(t.arg && *t.arg == "Worker");
        }
    }
    assert(found_format);
    assert(found_filter);
}

void test_hex_actor_id() {
    auto tokens = Lexer::tokenize("/actor 0x123 show");
    bool found_id = false;
    for (auto& t : tokens) {
        if (t.value == "0x123") found_id = true;
    }
    assert(found_id);
}

void test_quoted_string() {
    auto tokens = Lexer::tokenize("/actor \"My Worker 3\" show");
    bool found = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Parameter && t.value == "My Worker 3") found = true;
    }
    assert(found);
}

void test_escaped_quotes() {
    auto tokens = Lexer::tokenize("/actor \"hello\\nworld\" show");
    bool found = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Parameter && t.value == "hello\nworld") found = true;
    }
    assert(found);
}

void test_empty_input() {
    auto tokens = Lexer::tokenize("");
    assert(tokens.size() == 1);
    assert(tokens[0].type == TokenType::Eof);
}

void test_whitespace_only() {
    auto tokens = Lexer::tokenize("   \t  \n  ");
    assert(tokens.size() == 1);
    assert(tokens[0].type == TokenType::Eof);
}

void test_system_stats() {
    auto tokens = Lexer::tokenize("/system stats");
    assert(tokens.size() == 3);
    assert(tokens[0].value == "/");
    assert(tokens[1].value == "system");
    assert(tokens[2].value == "stats");
}

void test_monitor_start_with_filter() {
    auto tokens = Lexer::tokenize("/monitor start --filter EchoActor");
    bool found_monitor = false, found_start = false, found_filter = false;
    for (auto& t : tokens) {
        if (t.value == "monitor") found_monitor = true;
        if (t.value == "start") found_start = true;
        if (t.type == TokenType::FlagWithArg && t.value == "filter") {
            found_filter = true;
            assert(t.arg && *t.arg == "EchoActor");
        }
    }
    assert(found_monitor);
    assert(found_start);
    assert(found_filter);
}

int main() {
    test_simple_keywords();
    test_leading_slash();
    test_flags();
    test_flag_with_arg();
    test_hex_actor_id();
    test_quoted_string();
    test_escaped_quotes();
    test_empty_input();
    test_whitespace_only();
    test_system_stats();
    test_monitor_start_with_filter();
    printf("test_lexer: PASSED\n");
    return 0;
}
```

- [ ] **Step 5: Add source and test to CMakeLists.txt**

Add `src/cli/lexer.cpp` to the hpactor_lib sources. Add `tests/cli/test_lexer.cpp` as a test target.

- [ ] **Step 6: Build and run the test**

```bash
ninja -C build && ./build/tests/test_lexer
```

Expected: `test_lexer: PASSED`

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cli/token.hpp include/hpactor/cli/lexer.hpp src/cli/lexer.cpp tests/cli/test_lexer.cpp CMakeLists.txt
git commit -m "feat: add CLI lexer — whitespace tokenizer with flag/option support"
```

---

### Task 3: Implement CommandNode — Trie-Based Command Registry

**Files:**
- Create: `include/hpactor/cli/command_node.hpp`
- Create: `src/cli/command_node.cpp`
- Create: `tests/cli/test_command_node.cpp`

- [ ] **Step 1: Write the CommandNode header**

Create `include/hpactor/cli/command_node.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/types/types.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

struct CommandContext;

struct CommandNode {
    std::string keyword;
    std::string help_text;
    bool is_parameter = false;  // true for <id>, <filter>, etc.

    // Leaf action — set only on terminal nodes.
    std::function<result<void>(CommandContext&)> execute;

    // Ordered children for deterministic traversal.
    std::vector<std::unique_ptr<CommandNode>> children;

    // Builder API
    CommandNode* add_child(std::string kw, std::string help, bool is_param = false);

    // Lookup child by token. If is_parameter, matches any token
    // that is not itself a child keyword, and stores the matched value.
    CommandNode* find_child(const std::string& token, std::string& param_value) const;

    // Generate help text for this node's children.
    std::string help(int indent = 0) const;

    // Suggest closest match for typos (Levenshtein distance ≤ 2).
    std::string suggest(const std::string& token) const;
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 2: Write the CommandNode implementation**

Create `src/cli/command_node.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/command_node.hpp>
#include <algorithm>
#include <string>

namespace hpactor {
namespace cli {

CommandNode* CommandNode::add_child(std::string kw, std::string help, bool is_param) {
    auto node = std::make_unique<CommandNode>();
    node->keyword = std::move(kw);
    node->help_text = std::move(help);
    node->is_parameter = is_param;
    CommandNode* ptr = node.get();
    children.push_back(std::move(node));
    return ptr;
}

CommandNode* CommandNode::find_child(const std::string& token,
                                      std::string& param_value) const {
    // Exact keyword match first.
    for (auto& child : children) {
        if (!child->is_parameter && child->keyword == token) {
            return child.get();
        }
    }
    // Parameter match: any child with is_parameter=true matches any token.
    for (auto& child : children) {
        if (child->is_parameter) {
            param_value = token;
            return child.get();
        }
    }
    return nullptr;
}

static int levenshtein(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    // Small strings — simple DP on stack is fine.
    int d[32][32];
    for (size_t i = 0; i <= m; ++i) d[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) d[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
        }
    }
    return d[m][n];
}

std::string CommandNode::suggest(const std::string& token) const {
    std::string best;
    int best_dist = 999;
    for (auto& child : children) {
        if (child->is_parameter) continue;
        int dist = levenshtein(token, child->keyword);
        if (dist <= 2 && dist < best_dist) {
            best_dist = dist;
            best = child->keyword;
        }
    }
    return best;
}

std::string CommandNode::help(int indent) const {
    std::string out;
    std::string pad(static_cast<size_t>(indent), ' ');
    for (auto& child : children) {
        out += pad;
        if (child->is_parameter) {
            out += "<" + child->keyword + ">";
        } else {
            out += child->keyword;
        }
        out += "  —  " + child->help_text + "\n";

        if (!child->children.empty()) {
            out += child->help(indent + 2);
        }
    }
    return out;
}

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 3: Write the unit test**

Create `tests/cli/test_command_node.cpp`:

```cpp
#include <hpactor/cli/command_node.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

void test_tree_building() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    assert(root.children.size() == 2);
    assert(root.children[0]->keyword == "actor");
    assert(root.children[1]->keyword == "system");
}

void test_exact_match() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    std::string param;
    auto* node = root.find_child("actor", param);
    assert(node != nullptr);
    assert(node->keyword == "actor");
}

void test_missing() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");

    std::string param;
    auto* node = root.find_child("bogus", param);
    assert(node == nullptr);
}

void test_parameter_node() {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    actor->add_child("<id>", "Target actor ID", /*is_param=*/true);

    std::string param;
    auto* actor_node = root.find_child("actor", param);
    assert(actor_node != nullptr);

    auto* id_node = actor_node->find_child("0x123", param);
    assert(id_node != nullptr);
    assert(param == "0x123");
    assert(id_node->is_parameter);
}

void test_parameter_node_matches_any_non_keyword() {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", /*is_param=*/true);
    id_node->add_child("show", "Display actor metadata");

    // "5" is not a child keyword, so it matches <id>
    std::string param;
    auto* match = actor->find_child("5", param);
    assert(match != nullptr);
    assert(match->is_parameter);
    assert(param == "5");
}

void test_nested_commands() {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", /*is_param=*/true);
    id_node->add_child("show", "Display actor metadata");
    id_node->add_child("kill", "Terminate actor");

    // Full traversal
    std::string p1, p2;
    auto* n1 = root.find_child("actor", p1);
    assert(n1 != nullptr);
    auto* n2 = n1->find_child("5", p2);
    assert(n2 != nullptr);
    assert(p2 == "5");
    auto* n3 = n2->find_child("show", p1);
    assert(n3 != nullptr);
    assert(n3->keyword == "show");
}

void test_suggest() {
    CommandNode root{"", "root"};
    root.add_child("show", "Display actor metadata");
    root.add_child("list", "List actors");

    auto sug = root.suggest("shwo");
    assert(sug == "show");

    auto sug2 = root.suggest("lisst");
    assert(sug2 == "list");
}

void test_help_text() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    auto text = root.help();
    assert(text.find("actor") != std::string::npos);
    assert(text.find("Actor operations") != std::string::npos);
    assert(text.find("system") != std::string::npos);
}

int main() {
    test_tree_building();
    test_exact_match();
    test_missing();
    test_parameter_node();
    test_parameter_node_matches_any_non_keyword();
    test_nested_commands();
    test_suggest();
    test_help_text();
    printf("test_command_node: PASSED\n");
    return 0;
}
```

- [ ] **Step 4: Build and run the test**

```bash
ninja -C build && ./build/tests/test_command_node
```

Expected: `test_command_node: PASSED`

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cli/command_node.hpp src/cli/command_node.cpp tests/cli/test_command_node.cpp CMakeLists.txt
git commit -m "feat: add CommandNode trie with fuzzy suggestion and help text"
```

---

### Task 4: Implement CommandContext

**Files:**
- Create: `include/hpactor/cli/command_context.hpp`

- [ ] **Step 1: Write the CommandContext header**

Create `include/hpactor/cli/command_context.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/types/types.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliActor;
class OutputFormatter;

struct CommandContext {
    std::vector<std::string> args;
    std::map<std::string, std::string> params;
    ActorSystem* system = nullptr;
    CliActor* cli_actor = nullptr;
    OutputFormatter* output = nullptr;
    bool paged = false;
    uint32_t page_size = 50;
    std::string format = "pretty";

    bool has_flag(const std::string& name) const {
        auto it = params.find(name);
        return it != params.end() && (it->second == "true" || it->second.empty());
    }

    std::optional<std::string> get_param(const std::string& name) const {
        auto it = params.find(name);
        if (it != params.end()) return it->second;
        return std::nullopt;
    }
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/cli/command_context.hpp
git commit -m "feat: add CommandContext — execution context for CLI commands"
```

---

### Task 5: Implement OutputFormatters (Pretty, JSON, Tabular)

**Files:**
- Create: `include/hpactor/cli/output_formatter.hpp`
- Create: `include/hpactor/cli/pretty_formatter.hpp`
- Create: `include/hpactor/cli/json_formatter.hpp`
- Create: `include/hpactor/cli/tabular_formatter.hpp`
- Create: `src/cli/pretty_formatter.cpp`
- Create: `src/cli/json_formatter.cpp`
- Create: `src/cli/tabular_formatter.cpp`
- Create: `tests/cli/test_formatters.cpp`

- [ ] **Step 1: Write OutputFormatter interface**

Create `include/hpactor/cli/output_formatter.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

struct TreeNode {
    std::string name;
    std::string description;
    std::vector<TreeNode> children;
};

class OutputFormatter {
public:
    virtual ~OutputFormatter() = default;

    virtual void header(const std::string& title) = 0;
    virtual void table(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) = 0;
    virtual void key_value(const std::map<std::string, std::string>& pairs) = 0;
    virtual void tree(const TreeNode& root) = 0;
    virtual void raw(const std::string& text) = 0;
    virtual void error(const std::string& message) = 0;
    virtual std::string finalize() = 0;

    static std::unique_ptr<OutputFormatter> create(const std::string& format);
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 2: Write PrettyFormatter**

Create `include/hpactor/cli/pretty_formatter.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/output_formatter.hpp>

namespace hpactor {
namespace cli {

class PrettyFormatter : public OutputFormatter {
public:
    void header(const std::string& title) override;
    void table(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows) override;
    void key_value(const std::map<std::string, std::string>& pairs) override;
    void tree(const TreeNode& root) override;
    void raw(const std::string& text) override;
    void error(const std::string& message) override;
    std::string finalize() override;

private:
    std::string buffer_;
    int columns_ = 80;

    static std::string dim(const std::string& s);
    static std::string bold(const std::string& s);
    static std::string cyan(const std::string& s);
    static std::string green(const std::string& s);
    static std::string red(const std::string& s);
    static std::string pad_right(const std::string& s, size_t width);
    static std::string horizontal_rule(size_t width);
};

}  // namespace cli
}  // namespace hpactor
```

Create `src/cli/pretty_formatter.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/pretty_formatter.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace hpactor {
namespace cli {

// ANSI helpers
std::string PrettyFormatter::dim(const std::string& s)   { return "\033[2m" + s + "\033[0m"; }
std::string PrettyFormatter::bold(const std::string& s)  { return "\033[1m" + s + "\033[0m"; }
std::string PrettyFormatter::cyan(const std::string& s)  { return "\033[36m" + s + "\033[0m"; }
std::string PrettyFormatter::green(const std::string& s) { return "\033[32m" + s + "\033[0m"; }
std::string PrettyFormatter::red(const std::string& s)   { return "\033[31m" + s + "\033[0m"; }

std::string PrettyFormatter::pad_right(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

std::string PrettyFormatter::horizontal_rule(size_t width) {
    return std::string(width, '─');
}

void PrettyFormatter::header(const std::string& title) {
    auto* cols_env = std::getenv("COLUMNS");
    if (cols_env) columns_ = std::atoi(cols_env);
    if (columns_ < 40) columns_ = 80;

    buffer_ += "\n" + bold(title) + "\n";
    buffer_ += dim(horizontal_rule(static_cast<size_t>(columns_))) + "\n";
}

void PrettyFormatter::table(const std::vector<std::string>& cols,
                            const std::vector<std::vector<std::string>>& rows) {
    if (cols.empty()) return;

    // Calculate column widths
    std::vector<size_t> widths(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) {
        widths[i] = cols[i].size();
    }
    for (auto& row : rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    // Header row
    for (size_t i = 0; i < cols.size(); ++i) {
        buffer_ += bold(pad_right(cols[i], widths[i] + 2));
    }
    buffer_ += "\n";

    // Separator
    for (size_t i = 0; i < cols.size(); ++i) {
        buffer_ += dim(pad_right(std::string(widths[i], '─'), widths[i] + 2));
    }
    buffer_ += "\n";

    // Data rows
    for (auto& row : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            buffer_ += pad_right(i < row.size() ? row[i] : "-", widths[i] + 2);
        }
        buffer_ += "\n";
    }
}

void PrettyFormatter::key_value(const std::map<std::string, std::string>& pairs) {
    size_t max_key = 0;
    for (auto& [k, v] : pairs) max_key = std::max(max_key, k.size());

    for (auto& [k, v] : pairs) {
        buffer_ += "  " + cyan(pad_right(k, max_key + 2)) + green(v) + "\n";
    }
}

void PrettyFormatter::tree(const TreeNode& root) {
    std::function<void(const TreeNode&, int, bool)> print_node =
        [&](const TreeNode& node, int depth, bool last) {
        std::string indent(depth * 2, ' ');
        if (depth > 0) {
            buffer_ += indent + (last ? "└─ " : "├─ ");
        }
        buffer_ += bold(node.name);
        if (!node.description.empty()) {
            buffer_ += "  " + dim(node.description);
        }
        buffer_ += "\n";
        for (size_t i = 0; i < node.children.size(); ++i) {
            print_node(node.children[i], depth + 1, i == node.children.size() - 1);
        }
    };
    print_node(root, 0, true);
}

void PrettyFormatter::raw(const std::string& text) {
    buffer_ += text;
    if (!text.empty() && text.back() != '\n') buffer_ += '\n';
}

void PrettyFormatter::error(const std::string& message) {
    buffer_ += red("Error: ") + message + "\n";
}

std::string PrettyFormatter::finalize() {
    return std::move(buffer_);
}

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 3: Write JsonFormatter**

Create `include/hpactor/cli/json_formatter.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/output_formatter.hpp>
#include <sstream>

namespace hpactor {
namespace cli {

class JsonFormatter : public OutputFormatter {
public:
    void header(const std::string& title) override;
    void table(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows) override;
    void key_value(const std::map<std::string, std::string>& pairs) override;
    void tree(const TreeNode& root) override;
    void raw(const std::string& text) override;
    void error(const std::string& message) override;
    std::string finalize() override;

private:
    std::string buf_;

    static std::string json_escape(const std::string& s);
    void json_tree(const TreeNode& node);
};

}  // namespace cli
}  // namespace hpactor
```

Create `src/cli/json_formatter.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/json_formatter.hpp>

namespace hpactor {
namespace cli {

std::string JsonFormatter::json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;
        }
    }
    out += '"';
    return out;
}

void JsonFormatter::header(const std::string& title) {
    // No-op in JSON — the object structure conveys sectioning.
    (void)title;
}

void JsonFormatter::table(const std::vector<std::string>& columns,
                          const std::vector<std::vector<std::string>>& rows) {
    buf_ += "[";
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r > 0) buf_ += ",";
        buf_ += "{";
        for (size_t c = 0; c < columns.size() && c < rows[r].size(); ++c) {
            if (c > 0) buf_ += ",";
            buf_ += json_escape(columns[c]);
            buf_ += ":";
            buf_ += json_escape(rows[r][c]);
        }
        buf_ += "}";
    }
    buf_ += "]";
}

void JsonFormatter::key_value(const std::map<std::string, std::string>& pairs) {
    buf_ += "{";
    bool first = true;
    for (auto& [k, v] : pairs) {
        if (!first) buf_ += ",";
        first = false;
        buf_ += json_escape(k);
        buf_ += ":";
        buf_ += json_escape(v);
    }
    buf_ += "}";
}

void JsonFormatter::tree(const TreeNode& root) {
    buf_ += "{";
    buf_ += json_escape("name") + ":" + json_escape(root.name) + ",";
    buf_ += json_escape("description") + ":" + json_escape(root.description) + ",";
    buf_ += json_escape("children") + ":";
    json_tree(root);
    buf_ += "}";
}

void JsonFormatter::json_tree(const TreeNode& node) {
    buf_ += "[";
    for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) buf_ += ",";
        tree(node.children[i]);
    }
    buf_ += "]";
}

void JsonFormatter::raw(const std::string& text) {
    buf_ += json_escape(text);
}

void JsonFormatter::error(const std::string& message) {
    buf_ += "{\"error\":" + json_escape(message) + "}";
}

std::string JsonFormatter::finalize() {
    return std::move(buf_);
}

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 4: Write TabularFormatter**

Create `include/hpactor/cli/tabular_formatter.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/output_formatter.hpp>

namespace hpactor {
namespace cli {

class TabularFormatter : public OutputFormatter {
public:
    void header(const std::string& title) override;
    void table(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows) override;
    void key_value(const std::map<std::string, std::string>& pairs) override;
    void tree(const TreeNode& root) override;
    void raw(const std::string& text) override;
    void error(const std::string& message) override;
    std::string finalize() override;

private:
    std::string buffer_;
};

}  // namespace cli
}  // namespace hpactor
```

Create `src/cli/tabular_formatter.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/tabular_formatter.hpp>
#include <algorithm>

namespace hpactor {
namespace cli {

void TabularFormatter::header(const std::string& title) {
    buffer_ += "# " + title + "\n";
}

void TabularFormatter::table(const std::vector<std::string>& cols,
                             const std::vector<std::vector<std::string>>& rows) {
    if (cols.empty()) return;

    std::vector<size_t> widths(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) widths[i] = cols[i].size();
    for (auto& row : rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    // Header
    for (size_t i = 0; i < cols.size(); ++i) {
        std::string pad(widths[i] - cols[i].size(), ' ');
        buffer_ += cols[i] + pad + "  ";
    }
    buffer_ += "\n";

    // Data
    for (auto& row : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            std::string val = i < row.size() ? row[i] : "-";
            std::string pad(widths[i] > val.size() ? widths[i] - val.size() : 0, ' ');
            buffer_ += val + pad + "  ";
        }
        buffer_ += "\n";
    }
}

void TabularFormatter::key_value(const std::map<std::string, std::string>& pairs) {
    for (auto& [k, v] : pairs) {
        buffer_ += k + ": " + v + "\n";
    }
}

void TabularFormatter::tree(const TreeNode& root) {
    std::function<void(const TreeNode&, int)> print =
        [&](const TreeNode& node, int depth) {
        std::string indent(static_cast<size_t>(depth) * 2, ' ');
        buffer_ += indent + node.name;
        if (!node.description.empty()) buffer_ += "  # " + node.description;
        buffer_ += "\n";
        for (auto& child : node.children) print(child, depth + 1);
    };
    print(root, 0);
}

void TabularFormatter::raw(const std::string& text) {
    buffer_ += text;
    if (!text.empty() && text.back() != '\n') buffer_ += '\n';
}

void TabularFormatter::error(const std::string& message) {
    buffer_ += "ERROR: " + message + "\n";
}

std::string TabularFormatter::finalize() {
    return std::move(buffer_);
}

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 5: Write OutputFormatter factory and unit tests**

Create `tests/cli/test_formatters.cpp`:

```cpp
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/cli/json_formatter.hpp>
#include <hpactor/cli/tabular_formatter.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

void test_pretty_key_value() {
    PrettyFormatter f;
    f.key_value({{"State", "Running"}, {"Uptime", "12m 03s"}});
    auto out = f.finalize();
    assert(out.find("State") != std::string::npos);
    assert(out.find("Running") != std::string::npos);
}

void test_pretty_table() {
    PrettyFormatter f;
    f.table({"ID", "Type", "State"},
            {{"0x0001", "EchoActor", "Running"},
             {"0x0002", "Worker", "Idle"}});
    auto out = f.finalize();
    assert(out.find("ID") != std::string::npos);
    assert(out.find("EchoActor") != std::string::npos);
    assert(out.find("Idle") != std::string::npos);
}

void test_pretty_header() {
    PrettyFormatter f;
    f.header("Actor 0x0005");
    auto out = f.finalize();
    assert(out.find("Actor 0x0005") != std::string::npos);
}

void test_pretty_error() {
    PrettyFormatter f;
    f.error("actor not found");
    auto out = f.finalize();
    assert(out.find("Error") != std::string::npos);
    assert(out.find("actor not found") != std::string::npos);
}

void test_json_key_value() {
    JsonFormatter f;
    f.key_value({{"actor_id", "5"}, {"state", "Running"}});
    auto out = f.finalize();
    assert(out.find("\"actor_id\"") != std::string::npos);
    assert(out.find("\"Running\"") != std::string::npos);
    assert(out[0] == '{');
}

void test_json_table() {
    JsonFormatter f;
    f.table({"id", "type"}, {{"1", "EchoActor"}});
    auto out = f.finalize();
    assert(out[0] == '[');
    assert(out.find("\"id\"") != std::string::npos);
    assert(out.find("\"EchoActor\"") != std::string::npos);
}

void test_json_error() {
    JsonFormatter f;
    f.error("something went wrong");
    auto out = f.finalize();
    assert(out.find("\"error\"") != std::string::npos);
}

void test_tabular_no_ansi() {
    TabularFormatter f;
    f.key_value({{"key", "value"}});
    auto out = f.finalize();
    // No ANSI escape codes
    assert(out.find("\033") == std::string::npos);
    assert(out.find("key: value") != std::string::npos);
}

void test_tabular_table() {
    TabularFormatter f;
    f.table({"ID", "Type"}, {{"1", "Echo"}, {"2", "Worker"}});
    auto out = f.finalize();
    assert(out.find("ID") != std::string::npos);
    assert(out.find("Echo") != std::string::npos);
    assert(out.find("\033") == std::string::npos);
}

void test_formatter_factory() {
    auto pf = OutputFormatter::create("pretty");
    assert(pf != nullptr);

    auto jf = OutputFormatter::create("json");
    assert(jf != nullptr);

    auto tf = OutputFormatter::create("tabular");
    assert(tf != nullptr);

    // Unknown format falls back to pretty
    auto df = OutputFormatter::create("bogus");
    assert(df != nullptr);
}

int main() {
    test_pretty_key_value();
    test_pretty_table();
    test_pretty_header();
    test_pretty_error();
    test_json_key_value();
    test_json_table();
    test_json_error();
    test_tabular_no_ansi();
    test_tabular_table();
    test_formatter_factory();
    printf("test_formatters: PASSED\n");
    return 0;
}
```

Add the factory method to `src/cli/pretty_formatter.cpp` (or a separate file):

```cpp
std::unique_ptr<OutputFormatter> OutputFormatter::create(const std::string& format) {
    if (format == "json")     return std::make_unique<JsonFormatter>();
    if (format == "tabular")  return std::make_unique<TabularFormatter>();
    return std::make_unique<PrettyFormatter>();  // default
}
```

- [ ] **Step 6: Build and run tests**

```bash
ninja -C build && ./build/tests/test_formatters
```

Expected: `test_formatters: PASSED`

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cli/output_formatter.hpp include/hpactor/cli/pretty_formatter.hpp include/hpactor/cli/json_formatter.hpp include/hpactor/cli/tabular_formatter.hpp src/cli/pretty_formatter.cpp src/cli/json_formatter.cpp src/cli/tabular_formatter.cpp tests/cli/test_formatters.cpp CMakeLists.txt
git commit -m "feat: add OutputFormatters — Pretty (ANSI), JSON, Tabular"
```

---

### Task 6: Implement Pager — Interactive Paging State Machine

**Files:**
- Create: `include/hpactor/cli/pager.hpp`
- Create: `src/cli/pager.cpp`
- Create: `tests/cli/test_pager.cpp`

- [ ] **Step 1: Write Pager header**

Create `include/hpactor/cli/pager.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <functional>
#include <string>

namespace hpactor {
namespace cli {

class OutputFormatter;

class Pager {
public:
    enum class Action { Next, Previous, Quit, Search, Goto, Unknown };

    Pager(uint32_t page_size);

    // Show one page. Calls render(offset, limit) to get the rows.
    // Returns true if there are more pages.
    bool show_page(uint32_t total_items,
                   std::function<void(uint32_t offset, uint32_t limit)> render,
                   OutputFormatter* output);

    // Parse user input after a page prompt.
    Action parse_input(const std::string& input, std::string& arg);

    // Current page number (1-based).
    uint32_t current_page() const { return current_offset_ / page_size_ + 1; }

    // Total pages.
    uint32_t total_pages() const;

    // Move to a specific page.
    void goto_page(uint32_t page);

    void next_page();
    void prev_page();

private:
    uint32_t page_size_;
    uint32_t current_offset_ = 0;
    uint32_t total_items_ = 0;
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 2: Write Pager implementation**

Create `src/cli/pager.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <algorithm>
#include <charconv>
#include <cstdio>

namespace hpactor {
namespace cli {

Pager::Pager(uint32_t page_size) : page_size_(page_size) {}

bool Pager::show_page(uint32_t total_items,
                      std::function<void(uint32_t offset, uint32_t limit)> render,
                      OutputFormatter* output) {
    total_items_ = total_items;
    uint32_t start = current_offset_;
    uint32_t end = std::min(start + page_size_, total_items_);

    render(start, end - start);

    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "── Page %u of %u (%u-%u of %u) ──",
                     current_page(), total_pages(), start + 1, end, total_items_);
    output->raw(std::string(buf, static_cast<size_t>(n)));
    output->raw("[n]ext, [p]rev, [q]uit, /search, g<num>");

    return end < total_items_;
}

uint32_t Pager::total_pages() const {
    return (total_items_ + page_size_ - 1) / page_size_;
}

void Pager::goto_page(uint32_t page) {
    if (page < 1) page = 1;
    if (page > total_pages()) page = total_pages();
    current_offset_ = (page - 1) * page_size_;
}

void Pager::next_page() {
    if (current_offset_ + page_size_ < total_items_) {
        current_offset_ += page_size_;
    }
}

void Pager::prev_page() {
    if (current_offset_ >= page_size_) {
        current_offset_ -= page_size_;
    }
}

Pager::Action Pager::parse_input(const std::string& input, std::string& arg) {
    if (input.empty()) return Action::Next;
    if (input == "n" || input == "next") return Action::Next;
    if (input == "p" || input == "prev") return Action::Previous;
    if (input == "q" || input == "quit") return Action::Quit;
    if (input == "f" || input == "first") { goto_page(1); return Action::Goto; }
    if (input == "l" || input == "last")  { goto_page(total_pages()); return Action::Goto; }
    if (!input.empty() && input[0] == '/') {
        arg = input.substr(1);
        return Action::Search;
    }
    if (!input.empty() && input[0] == 'g') {
        arg = input.substr(1);
        uint32_t page = 0;
        auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), page);
        if (ec == std::errc{}) goto_page(page);
        return Action::Goto;
    }
    return Action::Unknown;
}

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 3: Write unit test**

Create `tests/cli/test_pager.cpp`:

```cpp
#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

void test_page_calculation() {
    Pager pager(50);
    assert(pager.current_page() == 1);

    pager.goto_page(3);
    assert(pager.current_page() == 3);
}

void test_clamp() {
    Pager pager(50);
    // goto_page calls render internally tracking total, but for standalone
    // tests we just check clamping
    pager.goto_page(100);
    // total_pages is 0 with no items, so clamp to 1
    assert(pager.current_page() >= 1);
}

void test_next_prev() {
    Pager pager(10);
    // Simulate navigation
    pager.next_page();
    assert(pager.current_page() == 1);  // no items tracked, no movement
}

void test_parse_input() {
    Pager pager(50);
    std::string arg;

    assert(pager.parse_input("", arg) == Pager::Action::Next);
    assert(pager.parse_input("n", arg) == Pager::Action::Next);
    assert(pager.parse_input("next", arg) == Pager::Action::Next);
    assert(pager.parse_input("p", arg) == Pager::Action::Previous);
    assert(pager.parse_input("q", arg) == Pager::Action::Quit);
    assert(pager.parse_input("quit", arg) == Pager::Action::Quit);
    assert(pager.parse_input("/Worker", arg) == Pager::Action::Search);
    assert(arg == "Worker");
    assert(pager.parse_input("g5", arg) == Pager::Action::Goto);
}

void test_show_page_output() {
    Pager pager(3);
    PrettyFormatter fmt;

    std::string rendered;
    pager.show_page(10,
        [&](uint32_t offset, uint32_t limit) {
            char buf[64];
            snprintf(buf, sizeof(buf), "offset=%u limit=%u", offset, limit);
            rendered = buf;
        },
        &fmt);

    assert(rendered == "offset=0 limit=3");

    auto out = fmt.finalize();
    assert(out.find("Page 1 of 4") != std::string::npos);
    assert(out.find("[n]ext") != std::string::npos);
}

int main() {
    test_page_calculation();
    test_clamp();
    test_next_prev();
    test_parse_input();
    test_show_page_output();
    printf("test_pager: PASSED\n");
    return 0;
}
```

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build && ./build/tests/test_pager
```

Expected: `test_pager: PASSED`

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cli/pager.hpp src/cli/pager.cpp tests/cli/test_pager.cpp CMakeLists.txt
git commit -m "feat: add Pager — interactive paging state machine for list commands"
```

---

### Task 7: Add `to_metadata()`, `serialize_state()`, `mailbox_snapshot()` to AbstractActor

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` (for mailbox_snapshot)
- Modify: `src/actor/event_based_actor.cpp` (system dispatch for CLI tags)

- [ ] **Step 1: Read current AbstractActor header**

Read `include/hpactor/actor/abstract_actor.hpp`. Identify where virtual methods are declared and where private members are stored.

- [ ] **Step 2: Add to_metadata() interface**

After the existing virtual method declarations in `AbstractActor`, add:

```cpp
// Returns lightweight inspectable metadata. Called from own thread — safe.
virtual cli::ActorMetadata to_metadata() const;

// Returns opaque protobuf-serialized state blob. Default empty.
virtual bytes serialize_state() const { return {}; }

// Returns mailbox snapshot. Default empty.
virtual cli::MailboxSnapshot mailbox_snapshot() const { return {}; }
```

Add `#include <hpactor/cli/...>` — but wait, the CLI messages protobuf generates `cli_messages.pb.h` which contains `ActorMetadata` and `MailboxSnapshot`. Include that.

Actually, to avoid coupling `abstract_actor.hpp` to protobuf, define the metadata structs as plain C++ structs in a lightweight header:

Create `include/hpactor/cli/cli_types.hpp` (a minimal header without protobuf dependency):

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/types/types.hpp>
#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

struct ActorMetadata {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
    uint64_t incarnation = 0;
    uint64_t messages_processed = 0;
    uint64_t uptime_ms = 0;
    std::string behavior_name;
};

struct MailboxSnapshot {
    uint32_t depth = 0;
    uint64_t total_enqueued = 0;
    uint64_t total_dequeued = 0;
    uint64_t max_depth = 0;
    uint32_t high_priority_depth = 0;
};

struct ChildInfo {
    uint64_t actor_id = 0;
    std::string actor_type;
    std::string state;
};

}  // namespace cli
}  // namespace hpactor
```

Now `abstract_actor.hpp` includes `cli_types.hpp` (no protobuf) and declares the virtual methods. The `CliActor` converts between these native structs and protobuf when sending messages.

- [ ] **Step 3: Implement to_metadata() default**

In `src/actor/abstract_actor.cpp`, add:

```cpp
#include <hpactor/cli/cli_types.hpp>

cli::ActorMetadata AbstractActor::to_metadata() const {
    cli::ActorMetadata m;
    m.actor_id = id().value();
    m.actor_type = type_name().data();
    m.state = "unknown";
    m.incarnation = incarnation();
    return m;
}
```

- [ ] **Step 4: Implement mailbox_snapshot() override in MPSCActorMailbox-owning actors**

Read `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`. Add a public method:

```cpp
cli::MailboxSnapshot snapshot() const {
    cli::MailboxSnapshot s;
    s.depth = mailbox_.count();
    s.total_enqueued = total_enqueued_.load();
    s.total_dequeued = total_dequeued_.load();
    s.max_depth = max_depth_.load();
    return s;
}
```

In `EventBasedActor`, override `mailbox_snapshot()` to delegate to the mailbox:

```cpp
cli::MailboxSnapshot EventBasedActor::mailbox_snapshot() const {
    if (mailbox_) return mailbox_->snapshot();
    return {};
}
```

- [ ] **Step 5: Add system dispatch for CLI request tags in EventBasedActor::receive()**

Read `src/actor/event_based_actor.cpp`. After the existing system message dispatch (LinkMsg, UnlinkMsg, DownMsg handling), add intercept for CLI request tags:

```cpp
// Intercept CLI introspection requests
if (msg.type_tag() == TypeTag::InspectStateRequestTag) {
    // Deserialize and handle
    cli::InspectStateRequest req;
    req.ParseFromArray(msg.payload().data(), static_cast<int>(msg.payload().size()));

    cli::InspectStateReply reply;
    reply.mutable_metadata()->set_actor_id(id().value());
    // ... copy from to_metadata() ...
    if (req.include_mailbox()) {
        auto ms = mailbox_snapshot();
        // ... copy into reply ...
    }
    if (req.include_children()) {
        // enumerate children from context
    }
    if (req.include_state()) {
        auto blob = serialize_state();
        reply.set_state_blob(blob.data(), blob.size());
    }

    context()->reply(TypedMessage::create(TypeTag::InspectStateResponseTag, reply));
    return result<void>{};
}
```

Similarly for `KillRequestTag`:

```cpp
if (msg.type_tag() == TypeTag::KillRequestTag) {
    cli::KillRequest req;
    req.ParseFromArray(msg.payload().data(), static_cast<int>(msg.payload().size()));

    cli::KillReply reply;
    reply.set_success(true);
    reply.set_error_code(0);

    context()->reply(TypedMessage::create(TypeTag::KillResponseTag, reply));

    // Schedule termination after reply is sent
    set_exit_reason(exit_reason::kNormal);
    return result<void>{};
}
```

- [ ] **Step 6: Build and verify**

```bash
ninja -C build
```

Expected: clean build. Fix any missing includes or forward declarations.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cli/cli_types.hpp include/hpactor/actor/abstract_actor.hpp src/actor/abstract_actor.cpp include/hpactor/mailbox/mpsc_actor_mailbox.hpp include/hpactor/actor/event_based_actor.hpp src/actor/event_based_actor.cpp
git commit -m "feat: add to_metadata()/serialize_state()/mailbox_snapshot() + CLI request dispatch"
```

---

### Task 8: Implement CliActor — DaemonActor with Command Loop

**Files:**
- Create: `include/hpactor/cli/cli_actor.hpp`
- Create: `src/cli/cli_actor.cpp`

- [ ] **Step 1: Write the CliActor header**

Create `include/hpactor/cli/cli_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <memory>
#include <string>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliActor : public DaemonActor {
public:
    CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config);

    // DaemonActor interface: called repeatedly on dedicated thread.
    // Returns true to continue, false to stop (on /quit).
    bool run_once() override;

    // Called when daemon thread starts.
    void on_daemon_start() override;

    // Called when daemon thread stops.
    void on_daemon_stop() override;

    // Accessors for commands
    ActorSystem& system() { return system_; }
    const CliConfig& config() const { return config_; }
    OutputFormatter* formatter() { return formatter_.get(); }
    Pager* pager() { return pager_.get(); }

private:
    void build_command_tree();
    void execute_tokens(const std::vector<Token>& tokens);
    void print_prompt();
    void print_greeting();

    ActorSystem& system_;
    CliConfig config_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    bool running_ = true;
    bool first_call_ = true;
};

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 2: Write the CliActor implementation**

Create `src/cli/cli_actor.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <cstdio>
#include <iostream>
#include <unistd.h>

namespace hpactor {
namespace cli {

CliActor::CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config)
    : DaemonActor(ctx, system)
    , system_(system)
    , config_(config)
{
    formatter_ = OutputFormatter::create(config.default_format);
    pager_ = std::make_unique<Pager>(config.page_size);
    build_command_tree();
}

void CliActor::on_daemon_start() {
    print_greeting();
}

void CliActor::on_daemon_stop() {
    printf("\n[CLI session ended]\n");
}

void CliActor::print_greeting() {
    printf("HPActor CLI v1.0 — Type /help for available commands. /quit to exit.\n\n");
}

void CliActor::print_prompt() {
    printf("hpactor> ");
    fflush(stdout);
}

void CliActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");

    // /actor <id> ...
    auto* actor = root->add_child("actor", "Actor operations");
    auto* actor_id = actor->add_child("<id>", "Target actor ID", /*is_param=*/true);

    actor_id->add_child("show", "Display actor metadata, state, mailbox, and children")
        ->execute = [](CommandContext& ctx) -> result<void> {
        // Build InspectStateRequest and send
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID");
            return {};
        }
        // Send InspectStateRequest to target...
        ctx.output->header("Actor " + *id_str);
        ctx.output->key_value({{"Status", "pending..."}});
        return {};
    };

    actor_id->add_child("kill", "Terminate actor")
        ->execute = [](CommandContext& ctx) -> result<void> {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID");
            return {};
        }
        ctx.output->raw("kill " + *id_str + " — not yet implemented");
        return {};
    };

    // /actor list
    actor->add_child("list", "List all actors")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Actor List");
        ctx.output->raw("list — not yet implemented");
        return {};
    };

    // /system ...
    auto* sys = root->add_child("system", "System operations");
    sys->add_child("stats", "System statistics")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Statistics");
        ctx.output->raw("stats — not yet implemented");
        return {};
    };
    sys->add_child("memory", "Memory subsystem stats")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Memory");
        ctx.output->raw("memory — not yet implemented");
        return {};
    };
    sys->add_child("list", "List system actors")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Actors");
        ctx.output->raw("system list — not yet implemented");
        return {};
    };

    // /metrics ...
    auto* metrics = root->add_child("metrics", "Metrics operations");
    metrics->add_child("show", "Show current metrics snapshot")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Metrics");
        ctx.output->raw("metrics show — not yet implemented");
        return {};
    };

    // /topology ...
    auto* topo = root->add_child("topology", "Topology operations");
    topo->add_child("show", "Show topology tree")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Topology");
        ctx.output->raw("topology show — not yet implemented");
        return {};
    };

    // /help
    root->add_child("help", "Show available commands")->execute =
        [this](CommandContext& ctx) -> result<void> {
        ctx.output->header("Available Commands");
        ctx.output->raw(command_tree_->help());
        return {};
    };

    // /quit
    root->add_child("quit", "Exit the CLI")->execute =
        [this](CommandContext& ctx) -> result<void> {
        ctx.output->raw("Goodbye.");
        running_ = false;
        return {};
    };

    command_tree_ = std::move(root);
}

void CliActor::execute_tokens(const std::vector<Token>& tokens) {
    // Reopen formatter for each command
    formatter_ = OutputFormatter::create(config_.default_format);

    CommandContext ctx;
    ctx.system = &system_;
    ctx.cli_actor = this;
    ctx.output = formatter_.get();
    ctx.page_size = config_.page_size;

    // Walk the command tree
    CommandNode* node = command_tree_.get();

    size_t i = 0;
    // Skip leading "/" keyword
    if (i < tokens.size() && tokens[i].value == "/") ++i;

    for (; i < tokens.size(); ++i) {
        auto& tok = tokens[i];

        if (tok.type == TokenType::Eof) break;

        if (tok.type == TokenType::Flag) {
            ctx.params[tok.value] = "true";
            continue;
        }

        if (tok.type == TokenType::FlagWithArg) {
            ctx.params[tok.value] = tok.arg.value_or("true");
            if (tok.value == "format") {
                ctx.format = tok.arg.value_or("pretty");
                formatter_ = OutputFormatter::create(ctx.format);
                ctx.output = formatter_.get();
            }
            continue;
        }

        // Try to match as keyword or parameter
        std::string param_value;
        auto* child = node->find_child(tok.value, param_value);
        if (!child) {
            auto suggestion = node->suggest(tok.value);
            std::string err = "Unknown command '" + tok.value + "'";
            if (!suggestion.empty()) err += " — did you mean '" + suggestion + "'?";
            formatter_->error(err);
            printf("%s\n", formatter_->finalize().c_str());
            return;
        }

        if (child->is_parameter) {
            ctx.params[child->keyword] = param_value;
        }
        node = child;
    }

    // Execute leaf node
    if (node->execute) {
        node->execute(ctx);
    } else {
        // No execute — show help for this node
        if (!node->children.empty()) {
            formatter_->header("Available commands");
            formatter_->raw(node->help());
        }
    }

    printf("%s\n", formatter_->finalize().c_str());
}

bool CliActor::run_once() {
    if (!running_) return false;  // stop the daemon loop

    print_prompt();

    std::string line;
    if (!std::getline(std::cin, line)) {
        // EOF on stdin
        printf("\nGoodbye.\n");
        return false;  // stop the daemon loop
    }

    if (line.empty()) return true;

    auto tokens = Lexer::tokenize(line);
    execute_tokens(tokens);
    return true;
}

}  // namespace cli
}  // namespace hpactor
```

- [ ] **Step 3: Add to CMakeLists.txt and build**

Add `src/cli/cli_actor.cpp` to hpactor_lib sources.

```bash
ninja -C build
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cli/cli_actor.hpp src/cli/cli_actor.cpp CMakeLists.txt
git commit -m "feat: add CliActor — DaemonActor with command tree, lexer, and input loop"
```

---

### Task 9: Implement Command Handlers (show, kill, list, stats, memory)

**Files:**
- Create: `include/hpactor/cli/commands/show_command.hpp`
- Create: `include/hpactor/cli/commands/kill_command.hpp`
- Create: `include/hpactor/cli/commands/list_command.hpp`
- Create: `include/hpactor/cli/commands/system_stats_command.hpp`
- Create: `include/hpactor/cli/commands/system_memory_command.hpp`

- [ ] **Step 1: Write ShowCommand**

Create `include/hpactor/cli/commands/show_command.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/cli/command_context.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/cli_messages.pb.h>

namespace hpactor {
namespace cli {
namespace commands {

inline result<void> execute_show(CommandContext& ctx) {
    auto id_str = ctx.get_param("<id>");
    if (!id_str) {
        ctx.output->error("Missing actor ID (usage: /actor <id> show)");
        return {};
    }

    uint64_t raw_id = 0;
    auto [ptr, ec] = std::from_chars(id_str->data(), id_str->data() + id_str->size(), raw_id, 0);
    if (ec != std::errc{}) {
        ctx.output->error("Invalid actor ID: " + *id_str);
        return {};
    }
    ActorId target_id(raw_id);

    // Build InspectStateRequest
    InspectStateRequest req;
    req.set_target_actor_id(raw_id);
    req.set_include_state(true);
    req.set_include_mailbox(true);
    req.set_include_children(true);

    // Create a promise/future pair for the response
    // Use ActorSystem's request-response via RPC channel or direct send+callback.
    // For the synchronous CLI: send request, block on response with timeout.

    auto* system = ctx.system;
    auto* transport = system->transport();

    TypedMessage msg;
    msg.set_type_tag(TypeTag::InspectStateRequestTag);
    std::string payload = req.SerializeAsString();
    msg.set_payload(payload.data(), payload.size());

    // Send and block on response (use scoped_actor or blocking receive pattern)
    // For now: stubbed
    ctx.output->header("Actor " + *id_str);
    ctx.output->key_value({{"Status", "request sent"}, {"ID", *id_str}});

    return {};
}

}  // namespace commands
}  // namespace cli
}  // namespace hpactor
```

Create similar stubs for `kill_command.hpp`, `list_command.hpp`, `system_stats_command.hpp`, and `system_memory_command.hpp`. Each follows the pattern: parse args from `CommandContext`, build the appropriate protobuf request, send it, and format the response.

- [ ] **Step 2: Wire commands into CliActor::build_command_tree()**

Replace the inline lambdas in `src/cli/cli_actor.cpp` with calls to the command functions:

```cpp
#include <hpactor/cli/commands/show_command.hpp>
#include <hpactor/cli/commands/kill_command.hpp>
#include <hpactor/cli/commands/list_command.hpp>
#include <hpactor/cli/commands/system_stats_command.hpp>
#include <hpactor/cli/commands/system_memory_command.hpp>

// In build_command_tree():
actor_id->add_child("show", "...")->execute = commands::execute_show;
actor_id->add_child("kill", "...")->execute = commands::execute_kill;
actor->add_child("list", "...")->execute = commands::execute_list;
sys->add_child("stats", "...")->execute = commands::execute_system_stats;
sys->add_child("memory", "...")->execute = commands::execute_system_memory;
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cli/commands/*.hpp src/cli/cli_actor.cpp
git commit -m "feat: add CLI command handlers (show, kill, list, stats, memory)"
```

---

### Task 10: Integrate CliActor into ActorSystem

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `include/hpactor/config/topology_model.hpp`
- Modify: `src/config/toml_parser.cpp`

- [ ] **Step 1: Add CliConfig to ActorSystem::Config**

Read `include/hpactor/core/actor_system.hpp`. Find the `Config` struct. Add:

```cpp
#include <hpactor/cli/cli_config.hpp>
// ... in Config:
cli::CliConfig cli;
```

- [ ] **Step 2: Spawn CliActor during ActorSystem initialization**

Read `src/actor/actor_system.cpp`. Find where system actors (MetricsActor, SpawnReceiver) are spawned. Add:

```cpp
// Spawn CLI actor
if (config_.cli.enabled) {
    // CliActor takes (ActorContext*, ActorSystem&, CliConfig)
    cli_actor_ = spawn<cli::CliActor>(ctx, *this, config_.cli);
}
```

Add `cli_actor_` member to ActorSystem:

```cpp
std::shared_ptr<cli::CliActor> cli_actor_;
```

- [ ] **Step 3: Add TOML parsing for CLI config**

Read `src/config/toml_parser.cpp`. In the `[system]` section parsing, add:

```cpp
// Parse [system.cli] table
if (auto* cli_tbl = table["cli"].as_table()) {
    read_bool(cli_tbl->get("enabled"), system_def.cli.enabled);
    read_string(cli_tbl->get("listen_path"), system_def.cli.listen_path);
    read_uint16(cli_tbl->get("tcp_port"), system_def.cli.tcp_port);
    read_string(cli_tbl->get("default_format"), system_def.cli.default_format);
    read_uint32(cli_tbl->get("page_size"), system_def.cli.page_size);
}
```

Read `include/hpactor/config/topology_model.hpp`. Add `CliConfig` to `SystemDef`:

```cpp
cli::CliConfig cli;
```

- [ ] **Step 4: Build and verify**

```bash
ninja -C build
```

Expected: clean build. All existing 85 tests pass. New cli tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp include/hpactor/config/topology_model.hpp src/config/toml_parser.cpp
git commit -m "feat: integrate CliActor into ActorSystem with TOML config support"
```

---

### Task 11: Add `ENABLE_CLI` CMake Option

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add CMake option**

In root `CMakeLists.txt`, near other `ENABLE_*` options:

```cmake
option(ENABLE_CLI "Enable interactive CLI subsystem" ON)
```

- [ ] **Step 2: Conditionally compile CLI sources**

When `ENABLE_CLI=ON`:
- Add `src/cli/*.cpp` to hpactor_lib sources
- Add `tests/cli/*.cpp` to test targets
- Add `-DHPACTOR_ENABLE_CLI` compile definition

When `ENABLE_CLI=OFF`:
- Skip all CLI sources and tests
- `CliConfig::enabled` defaults to false regardless

- [ ] **Step 3: Build both configurations**

```bash
cmake -S . -B build -GNinja -DENABLE_CLI=ON && ninja -C build
cmake -S . -B build-no-cli -GNinja -DENABLE_CLI=OFF && ninja -C build-no-cli
```

Expected: both build cleanly.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "feat: add ENABLE_CLI CMake option"
```

---

### Task 12: Integration Test — Full CLI Actor Lifecycle

**Files:**
- Create: `tests/cli/test_cli_integration.cpp`

- [ ] **Step 1: Write integration test**

Create `tests/cli/test_cli_integration.cpp`:

```cpp
#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/cli/json_formatter.hpp>
#include <hpactor/cli/tabular_formatter.hpp>
#include <hpactor/cli/pager.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

void test_command_parsing_roundtrip() {
    // Lexer → CommandNode traversal → context population
    auto tokens = Lexer::tokenize("/actor 0x123 show --format json");

    // Build minimal tree
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", /*is_param=*/true);
    id_node->add_child("show", "Display actor metadata");

    // Traverse
    CommandContext ctx;
    CommandNode* node = &root;
    for (size_t i = 0; i < tokens.size(); ++i) {
        auto& tok = tokens[i];
        if (tok.type == TokenType::Eof) break;
        if (tok.type == TokenType::Flag) {
            ctx.params[tok.value] = "true";
            continue;
        }
        if (tok.type == TokenType::FlagWithArg) {
            ctx.params[tok.value] = tok.arg.value_or("true");
            continue;
        }
        std::string param;
        auto* child = node->find_child(tok.value, param);
        assert(child != nullptr);
        if (child->is_parameter) ctx.params[child->keyword] = param;
        node = child;
    }

    assert(ctx.params["<id>"] == "0x123");
    assert(ctx.params["format"] == "json");
    assert(node->keyword == "show");
}

void test_full_command_with_flags() {
    auto tokens = Lexer::tokenize("/actor list --format json --no-pager --filter Worker");

    // Verify parser sees all flags
    int flags = 0;
    int flag_with_args = 0;
    for (auto& t : tokens) {
        if (t.type == TokenType::Flag) flags++;
        if (t.type == TokenType::FlagWithArg) flag_with_args++;
    }
    assert(flags == 1);       // --no-pager
    assert(flag_with_args == 2);  // --format json, --filter Worker
}

void test_pager_with_formatter() {
    Pager pager(5);
    PrettyFormatter fmt;

    int call_count = 0;
    pager.show_page(12,
        [&](uint32_t offset, uint32_t limit) {
            call_count++;
            assert(offset == 0);
            assert(limit == 5);
        },
        &fmt);

    assert(call_count == 1);
    auto out = fmt.finalize();
    assert(out.find("Page 1 of 3") != std::string::npos);
}

void test_formatter_integration() {
    // Headers flow into the output buffer
    PrettyFormatter pf;
    pf.header("Test Header");
    pf.key_value({{"Key1", "Val1"}, {"Key2", "Val2"}});
    pf.raw("Raw text");

    auto out = pf.finalize();
    assert(out.find("Test Header") != std::string::npos);
    assert(out.find("Key1") != std::string::npos);
    assert(out.find("Val2") != std::string::npos);
    assert(out.find("Raw text") != std::string::npos);
}

void test_tokenize_all_commands() {
    // Verify each known command parses correctly
    struct TestCase {
        const char* input;
        int expected_non_eof;
    };

    TestCase cases[] = {
        {"/actor 5 show", 3},
        {"/actor 5 kill", 3},
        {"/actor list", 2},
        {"/system stats", 2},
        {"/system memory", 2},
        {"/metrics show", 2},
        {"/topology show", 2},
    };

    for (auto& tc : cases) {
        auto tokens = Lexer::tokenize(tc.input);
        int count = 0;
        for (auto& t : tokens) {
            if (t.type == TokenType::Eof) break;
            count++;
        }
        assert(count == tc.expected_non_eof);
    }
}

int main() {
    test_command_parsing_roundtrip();
    test_full_command_with_flags();
    test_pager_with_formatter();
    test_formatter_integration();
    test_tokenize_all_commands();
    printf("test_cli_integration: PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Build and run**

```bash
ninja -C build && ./build/tests/test_cli_integration
```

Expected: `test_cli_integration: PASSED`

- [ ] **Step 3: Commit**

```bash
git add tests/cli/test_cli_integration.cpp CMakeLists.txt
git commit -m "test: add full CLI integration test"
```

---

### Task 13: Run Full Test Suite & Final Verification

- [ ] **Step 1: Run all tests**

```bash
ninja -C build && find build/tests -type f -perm -111 -name 'test_*' | sort | while read t; do "$t" >/dev/null 2>&1 && echo "PASS: $(basename $t)" || echo "FAIL: $(basename $t)"; done
```

Expected: all 85 existing + 6 new CLI tests pass (91 total).

- [ ] **Step 2: Manual CLI smoke test (if build includes examples with CLI enabled)**

```bash
# Build with CLI enabled
cmake -S . -B build -GNinja -DENABLE_CLI=ON && ninja -C build

# Run an example with CLI piped input
echo "/help" | timeout 2 ./build/examples/01_echo_actor 2>&1 || true
echo "/quit" | timeout 2 ./build/examples/01_echo_actor 2>&1 || true
```

Expected: greeting banner, help output, clean exit without crash.

- [ ] **Step 3: Commit final changes**

```bash
git add -A
git commit -m "chore: finalize CLI interactive subsystem — all tests passing"
```

---

## Test Plan Summary

| Test | Task | What It Validates |
|------|------|-------------------|
| `test_lexer` | 2 | Tokenization: keywords, flags, options, quoted strings, escape sequences, empty input |
| `test_command_node` | 3 | Tree building, exact match, parameter nodes, nested traversal, fuzzy suggestion, help text |
| `test_formatters` | 5 | PrettyFormatter (ANSI key-value, table, header, error), JsonFormatter (valid JSON), TabularFormatter (no ANSI), factory |
| `test_pager` | 6 | Page calculation, parse_input (n/p/q/search/goto), show_page output |
| `test_cli_integration` | 12 | End-to-end: lex → parse → context population, flag extraction, pager+formatter, all known commands |
| `test_cli_actor_spawn` | 10 | CliActor spawns, builds command tree, accepts input (tested via ActorSystem integration) |
| `test_command_execution` | 9 | show, kill, list, stats, memory command handlers (stubbed; full impl in Phase 3) |

**Total new tests: ~45 test cases across 7 test suites**

---

## Spec Deviations (Documented)

The plan makes these intentional simplifications from the spec:

1. **Lexer: static `tokenize()` vs stateful iterator** — The spec defines `Lexer` with `next()`, `peek()`, `remainder()` streaming API. The plan implements `static std::vector<Token> tokenize(const std::string&)`. For CLI input lines (small strings), materializing all tokens upfront is simpler and has no perf impact. The spec's `TokenType::Slash` is folded into `TokenType::Keyword` with value `"/"`.

2. **YAML formatter deferred** — The spec lists YAML as a fourth output format. The plan implements Pretty, JSON, and Tabular. YAML formatting is deferred — it requires either a third-party library or significant implementation effort for correct YAML escaping. The `OutputFormatter::create()` factory treats `"yaml"` as `"pretty"` (graceful fallback).

3. **Pager constructor** — Spec: `Pager(uint32_t, OutputFormatter*)`. Plan: `Pager(uint32_t)` with `OutputFormatter*` passed per-call in `show_page()`. This avoids stateful formatter coupling since the formatter is recreated per command.

## Open Items

1. **Blocking request-response**: The `show` and `kill` commands need the CLI Actor to send a request and block on a response. Since CliActor has its own dedicated thread, it can use a `std::promise`/`std::future` pair with a timeout. Abstract this into a `send_and_wait()` helper.

2. **ActorRegistry cursor API**: `/actor list` depends on `ActorRegistry::enumerate(Cursor, limit)`. This may already exist or need to be added. Check `include/hpactor/core/actor_registry.hpp`.

3. **Remote attach (Phase 4)**: UDS socket listener in `CliActor::run_once()` adds complexity to the input loop. The spec defines the JSON wire protocol. This is deferred until all local commands work.

4. **`hpactor-cli` frontend tool**: The separate executable for remote attach is a thin layer over the JSON wire protocol. It links against `hpactor_lib` for protobuf types but not the full actor runtime.

5. **TOML config propagation**: The `[system.cli]` TOML table must flow from `TomlParser` → `TopologyModel::SystemDef` → `ActorSystem::Config` → `CliActor`. This path exists (other system config already flows this way) — verify during Task 10.

6. **HPACTOR_MESSAGE_WITH_ID**: The recent `f5c3533` commit adds `HPACTOR_MESSAGE_WITH_ID` for fixed-tag registration. CLI message types should register via this macro to ensure consistency with the existing message registration system.
