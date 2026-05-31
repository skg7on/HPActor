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

#include <cstdint>
#include <string>
#include <vector>

// Forward-declare linenoise C types to avoid pulling linenoise.h into public
// headers.
struct linenoiseCompletions;

namespace hpactor::cli {

struct CommandNode;

struct LineEditorConfig {
    std::string history_path;
    uint32_t history_max = 1000;
    bool multiline = false;
};

/// Result of tab-completion computation for a partial input buffer.
struct CompletionResult {
    std::string prefix;               ///< Full prefix to prepend to each match.
    std::vector<std::string> matches; ///< Matching keywords at the current
                                      ///< level.
};

/// Result of inline-hint computation for a partial input buffer.
struct HintResult {
    std::string text; ///< Remainder of the hinted keyword after typed prefix.
    bool active = false; ///< When false, no hint should be displayed.
};

class LineEditor {
  public:
    LineEditor(const LineEditorConfig& cfg, const CommandNode* root);
    ~LineEditor();

    LineEditor(const LineEditor&) = delete;
    LineEditor& operator=(const LineEditor&) = delete;

    std::string readline(const std::string& prompt);
    void add_history(const std::string& line) const;
    void load_history() const;
    void save_history() const;
    void set_root(const CommandNode* root) {
        root_ = root;
    }

    /// Walk the command tree to compute tab completions for \p buf.
    ///
    /// Tokenizes the buffer, walks the tree consuming exact and prefix
    /// matches, then collects matching children at the resolved node.
    /// The returned prefix includes the leading "/" and all consumed
    /// tokens so that callers only need to append each match.
    CompletionResult compute_completions(const std::string& buf) const;

    /// Walk the command tree to compute an inline hint for \p buf.
    ///
    /// Finds the first non-parameter child whose keyword starts with
    /// the partial token at the cursor. The returned text is the
    /// remainder of the keyword after the already-typed prefix.
    HintResult compute_hint(const std::string& buf) const;

  private:
    // linenoise completion callback (global, no ctx — uses current_ editor
    // pointer). Called on Tab to populate completions.
    static void on_completion(const char* buf, struct linenoiseCompletions* lc);

    // linenoise hints callback (global, no ctx — uses current_ editor pointer).
    // Called on each keystroke; returns gray hint string.
    static char* on_hints(const char* buf, int* color, int* bold);

    // linenoise free-hints callback (global). Frees the string returned by
    // on_hints.
    static void on_free_hints(void* hint);

    void install_callbacks();
    static std::vector<std::string> tokenize_partial(const std::string& buf);

    const CommandNode* root_;
    LineEditorConfig config_;
    bool callbacks_installed_ = false;

    // Global pointer to the one active LineEditor (CLI is single-instance).
    // Set by constructor, cleared by destructor.
    static LineEditor* current_;
};

} // namespace hpactor::cli