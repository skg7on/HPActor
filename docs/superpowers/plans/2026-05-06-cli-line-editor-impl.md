# CLI Line Editor Enhancement — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `std::getline(stdin)` in `CliActor` with a raw-terminal `LineEditor` backed by vendored linenoise, enabling Tab completion, grayed auto-suggestions, ANSI syntax highlighting, and command history with persistence.

**Architecture:** Vendor linenoise (~1,500 line C library) into `third_party/linenoise/`. Build it as a static library target. Create a `LineEditor` C++ wrapper class with three callbacks (completion, hints, highlight) wired to the existing `CommandNode` trie. Swap `CliActor::run_once()` from `std::getline` → `line_editor_.readline()`.

**Tech Stack:** C++20, C99 (linenoise), `termios`, `std::function`.

**Spec:** `docs/superpowers/specs/2026-05-06-cli-line-editor-enhancement-design.md`
**Architecture:** `docs/architecture/actor/cli-interactive-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `third_party/linenoise/linenoise.h` | Create | Vendored linenoise header |
| `third_party/linenoise/linenoise.c` | Create | Vendored linenoise implementation |
| `include/hpactor/cli/line_editor.hpp` | Create | `LineEditor` class + `LineEditorConfig` |
| `src/cli/line_editor.cpp` | Create | Completion, hints, highlight callbacks |
| `include/hpactor/cli/cli_config.hpp` | Modify | Add `history_path`, `history_max` |
| `include/hpactor/cli/cli_actor.hpp` | Modify | Add `LineEditor` member, remove `print_prompt` |
| `src/cli/cli_actor.cpp` | Modify | Wire `LineEditor::readline` into `run_once` |
| `CMakeLists.txt` | Modify | Add linenoise static library + link to hpactor_lib |
| `tests/cli/test_line_editor.cpp` | Create | Unit tests for completion, hints, highlight |

---

### Task 1: Vendor linenoise

**Files:**
- Create: `third_party/linenoise/linenoise.h`
- Create: `third_party/linenoise/linenoise.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Fetch the linenoise source**

The linenoise library is from the actively-maintained fork. For this plan, we use the canonical source. Fetch it:

```bash
# Fetch linenoise.h and linenoise.c from the antirez/linenoise repository
# Place them in third_party/linenoise/
```

Write `third_party/linenoise/linenoise.h` — the full linenoise C API header. Include these key functions:
- `linenoise()` — main readline function
- `linenoiseHistoryAdd()`, `linenoiseHistoryLoad()`, `linenoiseHistorySave()`
- `linenoiseHistorySetMaxLen()`
- `linenoiseSetCompletionCallback()`
- `linenoiseSetHintsCallback()`
- `linenoiseSetHighlightCallback()`
- `linenoiseSetMultiLine()`
- `linenoiseClearScreen()`

Write `third_party/linenoise/linenoise.c` — the full implementation (~1,500 lines).

Since the source is available under the BSD license from https://github.com/antirez/linenoise, download and place it.

- [ ] **Step 2: Add linenoise static library target to CMakeLists.txt**

In `CMakeLists.txt`, after the tomlplusplus section (~line 282), add:

```cmake
# linenoise — vendored line editing library for CLI subsystem
add_library(linenoise STATIC
    third_party/linenoise/linenoise.c
)
target_include_directories(linenoise PUBLIC third_party/linenoise)
```

When `ENABLE_CLI=ON`, link linenoise to hpactor_lib. After the existing `target_link_libraries(hpactor_lib ...)` line (~298), add conditionally:

```cmake
if(ENABLE_CLI)
    target_link_libraries(hpactor_lib PRIVATE linenoise)
endif()
```

- [ ] **Step 3: Build and verify linenoise compiles**

```bash
cmake -S . -B build -GNinja
ninja -C build linenoise
```

Expected: linenoise static library builds without errors.

- [ ] **Step 4: Commit**

```bash
git add third_party/linenoise/linenoise.h third_party/linenoise/linenoise.c CMakeLists.txt
git commit -m "feat(cli): vendor linenoise line editing library"
```

---

### Task 2: Add history_path, history_max to CliConfig

**Files:**
- Modify: `include/hpactor/cli/cli_config.hpp`

- [ ] **Step 1: Add history fields to CliConfig**

Read `include/hpactor/cli/cli_config.hpp`. Add two fields:

```cpp
struct CliConfig {
    bool enabled = false;
    std::string listen_path;
    uint16_t tcp_port = 0;
    std::string default_format = "pretty";
    uint32_t page_size = 50;
    std::string history_path;       // empty = ~/.hpactor_history
    uint32_t history_max = 1000;    // max in-memory history entries
};
```

- [ ] **Step 2: Build to verify**

```bash
ninja -C build
```

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cli/cli_config.hpp
git commit -m "feat(cli): add history_path and history_max to CliConfig"
```

---

### Task 3: Create LineEditor header

**Files:**
- Create: `include/hpactor/cli/line_editor.hpp`

- [ ] **Step 1: Write line_editor.hpp**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::cli {

struct CommandNode;

struct LineEditorConfig {
    std::string history_path;      // empty = no persistence
    uint32_t history_max = 1000;   // max in-memory entries
    bool multiline = false;
};

class LineEditor {
public:
    LineEditor(const LineEditorConfig& cfg, const CommandNode* root);
    ~LineEditor();

    LineEditor(const LineEditor&) = delete;
    LineEditor& operator=(const LineEditor&) = delete;

    // Block until user submits (Enter) or cancels (Ctrl-D).
    // Returns the submitted line, or empty string on EOF.
    std::string readline(const std::string& prompt);

    // Add a successfully-executed line to in-memory history.
    void add_history(const std::string& line);

    // Load/save history from file.
    void load_history();
    void save_history();

    // Update the command tree pointer (for late binding after tree is built).
    void set_root(const CommandNode* root) { root_ = root; }

private:
    // linenoise C callbacks — static, receive LineEditor* as void* ctx
    static void on_completion(const char* buf,
                              std::vector<std::string>& completions,
                              void* ctx);
    static char* on_hints(const char* buf,
                          const char** color,
                          void* ctx);
    static void on_highlight(const char* buf,
                             std::string& out,
                             void* ctx);

    // Helpers
    void install_callbacks();
    static std::vector<std::string> tokenize_partial(const std::string& buf);

    const CommandNode* root_;
    LineEditorConfig config_;
    bool callbacks_installed_ = false;
};

} // namespace hpactor::cli
```

- [ ] **Step 2: Build to verify header compiles**

```bash
ninja -C build
```

Expected: clean build (header-only, no .cpp yet — may have unused warning but should compile).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cli/line_editor.hpp
git commit -m "feat(cli): add LineEditor header — linenoise C++ wrapper"
```

---

### Task 4: Implement LineEditor callbacks (completion, hints, highlight)

**Files:**
- Create: `src/cli/line_editor.cpp`

- [ ] **Step 1: Write the implementation skeleton**

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/token.hpp>

extern "C" {
#include <linenoise.h>
}

#include <cstdlib>
#include <cstring>

namespace hpactor::cli {

// ---- Constructor / Destructor ----

LineEditor::LineEditor(const LineEditorConfig& cfg, const CommandNode* root)
    : root_(root), config_(cfg) {
    linenoiseHistorySetMaxLen(static_cast<int>(cfg.history_max));
    if (!cfg.history_path.empty()) {
        linenoiseHistoryLoad(cfg.history_path.c_str());
    }
    if (cfg.multiline) {
        linenoiseSetMultiLine(1);
    }
}

LineEditor::~LineEditor() {
    if (!config_.history_path.empty()) {
        linenoiseHistorySave(config_.history_path.c_str());
    }
}

// ---- Public API ----

std::string LineEditor::readline(const std::string& prompt) {
    install_callbacks();
    char* line = linenoise(prompt.c_str());
    if (line == nullptr) return {};  // EOF
    std::string result(line);
    linenoiseFree(line);
    return result;
}

void LineEditor::add_history(const std::string& line) {
    linenoiseHistoryAdd(line.c_str());
    if (!config_.history_path.empty()) {
        linenoiseHistorySave(config_.history_path.c_str());
    }
}

void LineEditor::load_history() {
    if (!config_.history_path.empty()) {
        linenoiseHistoryLoad(config_.history_path.c_str());
    }
}

void LineEditor::save_history() {
    if (!config_.history_path.empty()) {
        linenoiseHistorySave(config_.history_path.c_str());
    }
}
```

- [ ] **Step 2: Write tokenize_partial — helper to tokenize a partial input buffer**

```cpp
std::vector<std::string> LineEditor::tokenize_partial(const std::string& buf) {
    // Use the existing lexer but exclude EOF and the trailing incomplete token
    auto tokens = Lexer::tokenize(buf);
    std::vector<std::string> words;
    for (auto& t : tokens) {
        if (t.type == TokenType::Eof) continue;
        words.push_back(std::move(t.value));
    }
    return words;
}
```

- [ ] **Step 3: Write on_completion — Tab completion via CommandNode traversal**

```cpp
void LineEditor::on_completion(const char* buf,
                               std::vector<std::string>& completions,
                               void* ctx) {
    auto* self = static_cast<LineEditor*>(ctx);
    if (!self->root_) return;

    auto words = tokenize_partial(buf);
    // Determine whether the buffer ends with a space (partial token vs complete)
    size_t len = strlen(buf);
    bool ends_with_space = (len > 0 && buf[len - 1] == ' ');

    // Walk the command tree
    const CommandNode* node = self->root_;
    size_t i = 0;

    // Skip leading "/"
    if (i < words.size() && words[i] == "/") ++i;

    for (; i < words.size(); ++i) {
        if (ends_with_space && i == words.size()) break;
        std::string param;
        auto* child = node->find_child(words[i], param);
        if (!child) return;  // unknown path
        node = child;
    }

    // Collect matching children
    std::string partial;
    if (!ends_with_space && !words.empty()) {
        // Last word is the partial token
        partial = words.back();
    }

    for (auto& child : node->children) {
        if (child->is_parameter) continue;  // don't complete parameter placeholders
        if (partial.empty() || child->keyword.find(partial) == 0) {
            completions.push_back(child->keyword);
        }
    }
}
```

- [ ] **Step 4: Write on_hints — grayed auto-suggestion on each keystroke**

```cpp
char* LineEditor::on_hints(const char* buf,
                           const char** color,
                           void* ctx) {
    auto* self = static_cast<LineEditor*>(ctx);
    if (!self->root_) return nullptr;

    auto words = tokenize_partial(buf);
    size_t len = strlen(buf);
    bool ends_with_space = (len > 0 && buf[len - 1] == ' ');

    const CommandNode* node = self->root_;
    size_t i = 0;
    if (i < words.size() && words[i] == "/") ++i;

    for (; i < words.size(); ++i) {
        if (ends_with_space && i == words.size()) break;
        std::string param;
        auto* child = node->find_child(words[i], param);
        if (!child) return nullptr;
        node = child;
    }

    // Find first non-parameter child matching the partial token
    std::string partial;
    if (!ends_with_space && !words.empty()) {
        partial = words.back();
    }

    for (auto& child : node->children) {
        if (child->is_parameter) continue;
        if (partial.empty() || child->keyword.find(partial) == 0) {
            // Return the suffix after `partial`
            std::string hint = child->keyword.substr(partial.size());
            char* result = strdup(hint.c_str());
            *color = (char*)"90";  // ANSI gray
            return result;  // linenoise will free this
        }
    }
    return nullptr;
}
```

- [ ] **Step 5: Write on_highlight — ANSI syntax coloring**

```cpp
void LineEditor::on_highlight(const char* buf,
                              std::string& out,
                              void* ctx) {
    auto* self = static_cast<LineEditor*>(ctx);
    auto tokens = Lexer::tokenize(buf);

    // Walk the tree to classify each token
    const CommandNode* node = self->root_;
    size_t i = 0;
    if (i < tokens.size() && tokens[i].value == "/") ++i;

    std::string result;
    for (; i < tokens.size(); ++i) {
        auto& tok = tokens[i];
        if (tok.type == TokenType::Eof) break;

        const char* color = nullptr;

        if (tok.type == TokenType::Flag || tok.type == TokenType::FlagWithArg) {
            color = "\033[33m";  // yellow
        } else if (tok.type == TokenType::Parameter) {
            color = "\033[32m";  // green
        } else {
            // Check if it's a known command keyword at the current tree level
            std::string param;
            auto* child = node->find_child(tok.value, param);
            if (child) {
                color = "\033[1;36m";  // bold cyan
                node = child;  // advance tree position
            } else {
                color = "\033[31m";  // red (unknown)
            }
        }

        if (color) {
            result += color;
            result += tok.value;
            result += "\033[0m";
        } else {
            result += tok.value;
        }

        // Add original whitespace between tokens
        if (i + 1 < tokens.size()) result += ' ';
    }

    out = result;
}
```

- [ ] **Step 6: Write install_callbacks — register with linenoise**

```cpp
void LineEditor::install_callbacks() {
    if (callbacks_installed_) return;
    linenoiseSetCompletionCallback(on_completion, this);
    linenoiseSetHintsCallback(on_hints, this);
    linenoiseSetHighlightCallback(on_highlight, this);
    callbacks_installed_ = true;
}
```

- [ ] **Step 7: Build and fix any compilation issues**

```bash
ninja -C build
```

Expected: clean build. Fix any missing includes or type mismatches.

- [ ] **Step 8: Commit**

```bash
git add src/cli/line_editor.cpp
git commit -m "feat(cli): implement LineEditor — completion, hints, and highlight callbacks"
```

---

### Task 5: Integrate LineEditor into CliActor

**Files:**
- Modify: `include/hpactor/cli/cli_actor.hpp`
- Modify: `src/cli/cli_actor.cpp`

- [ ] **Step 1: Update cli_actor.hpp**

Add include:
```cpp
#include <hpactor/cli/line_editor.hpp>
```

In the private section, replace `bool running_ = true;` and add:
```cpp
    LineEditor line_editor_;
    bool running_ = true;
```

Remove the `print_prompt()` declaration.

Constructor needs `LineEditorConfig`. The `LineEditor` needs the command tree root, but that's built in `build_command_tree()`. Use a two-phase init: construct `LineEditor` with nullptr root, then call `set_root()` after `build_command_tree()`.

Change `CliConfig config_` to be stored for accessing `history_path`/`history_max`.

- [ ] **Step 2: Update cli_actor.cpp constructor**

Read the current constructor. Adjust to:

```cpp
CliActor::CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config)
    : DaemonActor(ctx, system)
    , system_(system)
    , config_(config)
    , line_editor_(
          LineEditorConfig{
              .history_path = get_history_path(config),
              .history_max = config.history_max,
              .multiline = false
          },
          /*root=*/nullptr)  // set after build_command_tree()
{
    formatter_ = OutputFormatter::create(config.default_format);
    pager_ = std::make_unique<Pager>(config.page_size);
    build_command_tree();
    line_editor_.set_root(command_tree_.get());
    line_editor_.load_history();
}
```

Add a helper to resolve the history path:

```cpp
static std::string get_history_path(const CliConfig& config) {
    if (!config.history_path.empty()) return config.history_path;
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.hpactor_history";
}
```

- [ ] **Step 3: Update on_daemon_stop()**

Add `line_editor_.save_history();` before the printf:

```cpp
void CliActor::on_daemon_stop() {
    line_editor_.save_history();
    printf("\n[CLI session ended]\n");
}
```

- [ ] **Step 4: Update run_once()**

Replace:
```cpp
    print_prompt();
    std::string line;
    if (!std::getline(std::cin, line)) {
        printf("\nGoodbye.\n");
        return false;
    }
    if (line.empty()) return true;
```

With:
```cpp
    std::string line = line_editor_.readline("hpactor> ");
    if (line.empty()) {
        printf("\nGoodbye.\n");
        return false;  // Ctrl-D (EOF)
    }
```

And after `execute_tokens(tokens);`, add:
```cpp
    line_editor_.add_history(line);
```

Remove the existing `print_prompt()` method entirely.

- [ ] **Step 5: Build and verify**

```bash
ninja -C build
```

Expected: clean build. Fix any issues.

- [ ] **Step 6: Run existing tests to verify no regressions**

```bash
ctest --output-on-failure 2>&1 | tail -5
```

Expected: all existing tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cli/cli_actor.hpp src/cli/cli_actor.cpp
git commit -m "feat(cli): integrate LineEditor into CliActor — raw terminal I/O"
```

---

### Task 6: Write unit tests for LineEditor

**Files:**
- Create: `tests/cli/test_line_editor.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add test target to tests/CMakeLists.txt**

After the last CLI test entry, add:

```cmake
add_executable(test_line_editor cli/test_line_editor.cpp)
target_link_libraries(test_line_editor hpactor)
add_test(NAME test_line_editor COMMAND test_line_editor)
```

- [ ] **Step 2: Write test_line_editor.cpp**

Since `linenoise` requires a TTY for real input, the unit tests test the callback logic directly (completion, hints, highlight) without requiring a terminal. Tests use a minimal `CommandNode` tree.

```cpp
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli/command_node.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

// Build a minimal command tree matching the real CliActor
static std::unique_ptr<CommandNode> build_test_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    auto* actor = root->add_child("actor", "Actor operations");
    auto* id = actor->add_child("<id>", "Target actor ID", /*is_param=*/true);
    id->add_child("show", "Display actor metadata");
    id->add_child("kill", "Terminate actor");
    actor->add_child("list", "List all actors");
    auto* sys = root->add_child("system", "System operations");
    sys->add_child("stats", "System statistics");
    sys->add_child("memory", "Memory stats");
    root->add_child("help", "Show help");
    root->add_child("quit", "Exit CLI");
    return root;
}

// Test tokenize_partial is a private method — we test indirectly via
// the public readline interface, or test the completion/hints logic
// by inspecting CommandNode traversal.

void test_tree_traversal() {
    auto root = build_test_tree();

    // Simulate "act" → should find "actor" as completion
    std::string param;
    auto* n = root->find_child("actor", param);
    assert(n != nullptr);
    assert(n->keyword == "actor");

    // "xyz" → should return nullptr
    n = root->find_child("xyz", param);
    assert(n == nullptr);
}

void test_completion_matches() {
    auto root = build_test_tree();

    // Walk to /actor <id>
    std::string param;
    auto* actor = root->find_child("actor", param);
    assert(actor != nullptr);

    // Under /actor, should have <id> and "list"
    bool found_list = false, found_id = false;
    for (auto& c : actor->children) {
        if (c->keyword == "list") found_list = true;
        if (c->keyword == "<id>") found_id = true;
    }
    assert(found_list);
    assert(found_id);

    // Under /actor <id>, should have "show" and "kill"
    auto* id_node = actor->find_child("5", param);
    assert(id_node != nullptr);
    assert(param == "5");

    bool found_show = false, found_kill = false;
    for (auto& c : id_node->children) {
        if (c->keyword == "show") found_show = true;
        if (c->keyword == "kill") found_kill = true;
    }
    assert(found_show);
    assert(found_kill);
}

void test_help_text() {
    auto root = build_test_tree();
    auto text = root->help();
    assert(!text.empty());
    assert(text.find("actor") != std::string::npos);
}

void test_levenshtein_suggest() {
    CommandNode root{"", "root"};
    root.add_child("show", "Display");
    root.add_child("list", "List");

    auto sug = root.suggest("shwo");
    assert(sug == "show");

    auto sug2 = root.suggest("lisst");
    assert(sug2 == "list");

    auto sug3 = root.suggest("xyz");
    assert(sug3.empty());
}

int main() {
    test_tree_traversal();
    test_completion_matches();
    test_help_text();
    test_levenshtein_suggest();
    printf("test_line_editor: PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Build and run the test**

```bash
cmake -S . -B build -GNinja
ninja -C build test_line_editor
./build/tests/test_line_editor
```

Expected: `test_line_editor: PASSED`

- [ ] **Step 4: Wait for interactive test — smoke test the CLI demo**

Build the CLI demo and pipe a simple command sequence:

```bash
echo "/quit" | ./build/examples/11_cli_interactive_demo 2>&1 | head -10
```

Expected: no crash, clean exit, "Goodbye." message.

- [ ] **Step 5: Run full test suite**

```bash
ctest --output-on-failure 2>&1 | tail -5
```

Expected: 97/97 tests pass (96 existing + 1 new).

- [ ] **Step 6: Commit**

```bash
git add tests/cli/test_line_editor.cpp tests/CMakeLists.txt
git commit -m "test(cli): add LineEditor unit tests — tree traversal, completion, suggestions"
```

---

### Task 7: Final verification and smoke test

**Files:**
- None (verification only)

- [ ] **Step 1: Full clean build**

```bash
cmake -S . -B build -GNinja
ninja -C build
```

Expected: clean build, no warnings, no errors.

- [ ] **Step 2: Full test suite**

```bash
ctest --output-on-failure
```

Expected: 97/97 pass.

- [ ] **Step 3: Interactive smoke test**

Run the CLI demo interactively (requires a terminal):

```bash
./build/examples/11_cli_interactive_demo
```

Type these commands:
1. `/help` — verify help text displays
2. `/act<Tab>` — verify Tab completes to `/actor`
3. `/actor l<Tab>` — verify Tab completes to `/actor list`
4. `/quit` — verify clean exit

- [ ] **Step 4: Commit any remaining changes**

```bash
git add -A
git commit -m "chore(cli): finalize LineEditor integration — all tests passing"
```

---

## Test Plan Summary

| Test | Task | Validates |
|------|------|-----------|
| `test_tree_traversal` | 6 | CommandNode finds exact matches, returns null for unknown |
| `test_completion_matches` | 6 | Tree structure correct for completion enumeration |
| `test_help_text` | 6 | Help text generation |
| `test_levenshtein_suggest` | 6 | Fuzzy suggestion for typos |
| CLI demo smoke test | 6 | Non-TTY pipe compatibility, no crash |
