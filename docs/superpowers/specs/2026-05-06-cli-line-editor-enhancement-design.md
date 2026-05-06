# CLI Line Editor Enhancement — Design Specification

**Date:** 2026-05-06
**Status:** Draft

---

## 1. Overview

Replace the current `std::getline(std::cin)` input loop in `CliActor` with a raw-terminal line editor backed by the vendored linenoise library. This enables: Tab completion, auto-suggestion (gray hints), command history with arrow-key navigation, Ctrl-R reverse search, and syntax highlighting — all without external runtime dependencies.

## 2. Motivation

The current `run_once()` blocks on buffered line-mode input. There is no way to:

- Recall a previous command (up arrow)
- Auto-complete a partial command (Tab)
- Navigate or edit mid-line (left/right arrows, Ctrl+A/E, etc.)
- See inline suggestions while typing

These are baseline expectations for any interactive CLI. The infrastructure for completion (trie-based `CommandNode`) and suggestion (Levenshtein `suggest()`) already exists — it just needs to be wired into a line editor that invokes callbacks while the user types.

## 3. Library: linenoise

Vendored from the actively-maintained fork `antirez/linenoise` (Apache 2.0 licensed variant), placed at `third_party/linenoise/linenoise.c` + `linenoise.h`.

**Key features we use:**
- `linenoiseSetCompletionCallback()` — called on Tab, receives partial buffer, fills completion vector
- `linenoiseSetHintsCallback()` — called on each keystroke, returns gray hint string
- `linenoiseSetHighlightCallback()` — called on each keystroke, returns ANSI-colorized input
- `linenoiseHistoryAdd()` / `linenoiseHistoryLoad()` / `linenoiseHistorySave()` — history management
- `linenoiseSetMultiLine()` — enables multi-line editing for long commands

**Build integration:** `linenoise.c` added as a static library target `linenoise` in CMake; `hpactor_lib` links it privately when `ENABLE_CLI=ON`.

## 4. C++ Wrapper: LineEditor

### 4.1 Header (`include/hpactor/cli/line_editor.hpp`)

```cpp
namespace hpactor::cli {

struct LineEditorConfig {
    std::string history_path;      // ~/.hpactor_history
    uint32_t history_max = 1000;
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

    // Add a successfully-executed line to history.
    void add_history(const std::string& line);

    // Load/save history from file.
    void load_history();
    void save_history();

private:
    static void on_completion(const char* buf,
                              std::vector<std::string>& completions,
                              void* ctx);
    static char* on_hints(const char* buf,
                          const char** color,
                          void* ctx);
    static void on_highlight(const char* buf,
                             std::string& out,
                             void* ctx);

    const CommandNode* root_;
    LineEditorConfig config_;
};

} // namespace hpactor::cli
```

### 4.2 Completion Algorithm

On Tab, linenoise calls `on_completion(partial_buffer, completions, ctx)`.

1. Tokenize `partial_buffer` with `Lexer::tokenize()`, excluding EOF and trailing incomplete token.
2. Walk `CommandNode` tree from root, consuming matched tokens.
3. At the current node, enumerate children whose keyword starts with the partial token.
4. Populate `completions` with matching keywords. If only one match, linenoise auto-replaces.
5. If the current node has children, also offer them as completions (sub-command discovery).

**Edge case:** When the partial buffer is empty or just `/`, complete from the root node's children (`actor`, `system`, `metrics`, `topology`, `help`, `quit`).

### 4.3 Auto-Suggestion Algorithm

On each keystroke, linenoise calls `on_hints(partial_buffer, &color, ctx)`.

1. Tokenize the partial buffer.
2. Walk the command tree as far as possible with the completed tokens.
3. The first child keyword of the current node becomes the hint for the next token.
4. If the partial ends mid-token, find the first child keyword starting with that prefix.
5. Returns the hint string (remaining characters to complete). The color hint (`90` = gray) is set via `*color`.

**Example:**
```
User types: /act
Hint shown: or           # completing "actor"
User types: /actor 1
Hint shown:  show        # completing "show" (actor 1 → first child)
```

### 4.4 Syntax Highlighting

On each keystroke, linenoise calls `on_highlight(buf, out, ctx)`.

1. Tokenize the full buffer.
2. For each token span, apply color based on token type:
   - Known command keywords (`actor`, `system`, `show`, `list`, etc.): bold cyan (`\033[1;36m`)
   - Parameters (IDs, names): green (`\033[32m`)
   - Flags (`--detail`, `--format`): yellow (`\033[33m`)
   - Flag values (`json`, `Worker`): default
   - Unknown tokens: red (`\033[31m`)
   - Errors / trailing unrecognized text: red (`\033[31m`)

Color codes are reset to `\033[0m` between tokens.

### 4.5 History

- **In-memory:** linenoise maintains an internal ring buffer (default 100 entries, configurable via `history_max`).
- **Persistence:** `load_history()` reads from `history_path` on construction. `save_history()` writes on destruction and on each `add_history()` call.
- **Navigation:** Up arrow = `linenoiseHistoryLoad()` previous entry; Down = next. Built into linenoise.
- **Ctrl-R:** Built into linenoise — incremental reverse search through history.
- **Duplicate suppression:** linenoise does not add consecutive identical lines.

## 5. CliActor Integration

### 5.1 Changes to `cli_actor.hpp`

- Add `#include <hpactor/cli/line_editor.hpp>`
- Add member: `LineEditor line_editor_;`
- Remove: `print_prompt()` method
- Remove: `bool first_call_` member (was already unused)

### 5.2 Changes to `cli_actor.cpp`

**Constructor:** Initialize `line_editor_` with `command_tree_.get()` and `CliConfig`-derived path:

```cpp
CliActor::CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      line_editor_(LineEditorConfig{
          .history_path = config.listen_path.empty()
              ? std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.hpactor_history"
              : std::string(),
          .history_max = 1000
      }, /* root passed after build_command_tree */) {
    formatter_ = OutputFormatter::create(config.default_format);
    pager_ = std::make_unique<Pager>(config.page_size);
    build_command_tree();
    line_editor_.set_root(command_tree_.get());  // set after tree is built
    line_editor_.load_history();
}
```

**`on_daemon_stop()`:** Call `line_editor_.save_history()` before the final printf.

**`run_once()`:** Replace `print_prompt()` + `std::getline()` with:

```cpp
bool CliActor::run_once() {
    if (!running_) return false;

    std::string line = line_editor_.readline("hpactor> ");
    if (line.empty()) {
        printf("\nGoodbye.\n");
        return false;  // EOF (Ctrl-D)
    }

    auto tokens = Lexer::tokenize(line);
    execute_tokens(tokens);
    line_editor_.add_history(line);
    return true;
}
```

**`execute_tokens()`:** No changes needed — receives the same token stream as before.

### 5.3 Pipe/Non-TTY Detection

If stdin is not a TTY (e.g., piped input or `--exec` mode), linenoise falls back to simple `fgets()`-based read. No raw terminal setup is attempted. This preserves the existing behavior for scripted/one-shot usage.

## 6. File Structure

| File | Action | Purpose |
|------|--------|---------|
| `include/hpactor/cli/line_editor.hpp` | Create | `LineEditor` class and `LineEditorConfig` |
| `src/cli/line_editor.cpp` | Create | Implementation (completion, hints, highlight callbacks) |
| `include/hpactor/cli/cli_actor.hpp` | Modify | Add `line_editor_` member, remove `print_prompt` |
| `src/cli/cli_actor.cpp` | Modify | Wire `LineEditor::readline` into `run_once` |
| `include/hpactor/cli/cli_config.hpp` | Modify | Add `history_path`, `history_max` fields |
| `third_party/linenoise/linenoise.c` | Create | Vendored library |
| `third_party/linenoise/linenoise.h` | Create | Vendored header |
| `third_party/linenoise/CMakeLists.txt` | Create | Static library target |
| `CMakeLists.txt` | Modify | Add `linenoise` target, link to `hpactor_lib` |

## 7. Concurrency

No new concurrency concerns. `LineEditor::readline()` blocks on stdin on the same dedicated daemon thread as the current `std::getline()` call. The `CommandNode` tree is read-only after construction — the completion/hints/highlight callbacks traverse it without locks.

## 8. Edge Cases

| Scenario | Handling |
|----------|----------|
| stdin is a pipe (not a TTY) | linenoise auto-detects; falls back to `fgets()` |
| Empty command line (Enter with no text) | `readline` returns empty string; CliActor re-prompts |
| Ctrl-D on empty line | `readline` returns empty; CliActor treats as EOF, sets `running_ = false` |
| Ctrl-D on non-empty line | linenoise clears the line (default behavior); no action |
| Ctrl-C | linenoise returns empty string; CliActor ignores (no crash) |
| History file not writable | `save_history()` silently fails; in-memory history still works |
| Very long history file | linenoise truncates to `history_max` on load |
| Terminal resize (SIGWINCH) | linenoise handles internally; refreshes display |
| Completion with no matches | linenoise does nothing (no bell) |
| Multiline paste | `multiline = false` by default; pasted newlines treated as Enter |

## 9. Test Plan

| Test | What It Validates |
|------|-------------------|
| `test_line_editor_completion_root` | Empty buffer → Tab → sees `actor`, `system`, `metrics`, etc. |
| `test_line_editor_completion_partial` | `/act` → Tab → completes to `/actor` |
| `test_line_editor_completion_subcommand` | `/actor 5 ` → Tab → sees `show`, `kill`, etc. |
| `test_line_editor_hints_basic` | `/act` → hint shows `or` (gray) |
| `test_line_editor_hints_subcommand` | `/actor 5 s` → hint shows `how` |
| `test_line_editor_history_add` | Execute command, `add_history`, verify saved to file |
| `test_line_editor_highlight` | Tokenize and colorize: keyword (cyan), parameter (green), flag (yellow) |
| `test_cli_line_editor_integration` | CliActor with LineEditor: pipe input, verify execution flow |
