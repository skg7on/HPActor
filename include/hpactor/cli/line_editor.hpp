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

/// \brief Configuration for the interactive line editor.
struct LineEditorConfig {
    /// \brief Path to the history file.
    std::string history_path;
    /// \brief Maximum number of in-memory history entries.
    uint32_t history_max = 1000;
    /// \brief Whether to enable multiline input mode.
    bool multiline = false;
};

/// \brief Result of tab-completion computation for a partial input buffer.
struct CompletionResult {
    /// \brief Full prefix to prepend to each match (includes the leading "/"
    ///        and all consumed tokens).
    std::string prefix;
    /// \brief Matching keywords at the current trie level.
    std::vector<std::string> matches;
};

/// \brief Result of inline-hint computation for a partial input buffer.
struct HintResult {
    /// \brief Remainder of the hinted keyword after the typed prefix.
    std::string text;
    /// \brief When false, no hint should be displayed.
    bool active = false;
};

/// \brief Interactive line editor wrapping linenoise with CLI-aware completion.
///
/// Provides readline with history, tab completion driven by the command tree,
/// and inline hints showing the remainder of partially-typed keywords.
///
/// \note Thread affinity: single-instance, used only on the CLI daemon thread.
class LineEditor {
  public:
    /// \brief Construct the line editor.
    ///
    /// \param[in] cfg Editor configuration (history path, limits, multiline).
    /// \param[in] root Non-owning pointer to the command tree root for
    ///                 completion and hint computation.
    LineEditor(const LineEditorConfig& cfg, const CommandNode* root);

    /// \brief Destructor. Clears the global current_ pointer.
    ~LineEditor();

    LineEditor(const LineEditor&) = delete;
    LineEditor& operator=(const LineEditor&) = delete;

    /// \brief Read a line of input from the user.
    ///
    /// \param[in] prompt Prompt string displayed to the user.
    /// \return The raw input line, or an empty string on EOF.
    std::string readline(const std::string& prompt);

    /// \brief Add a line to the in-memory history.
    ///
    /// \param[in] line The input line to record.
    void add_history(const std::string& line) const;

    /// \brief Load history from the configured history file.
    void load_history() const;

    /// \brief Save current history to the configured history file.
    void save_history() const;

    /// \brief Update the command tree root for completion and hints.
    ///
    /// \param[in] root Non-owning pointer to the new command tree root.
    void set_root(const CommandNode* root) {
        root_ = root;
    }

    /// \brief Walk the command tree to compute tab completions for \p buf.
    ///
    /// Tokenizes the buffer, walks the tree consuming exact and prefix
    /// matches, then collects matching children at the resolved node.
    /// The returned prefix includes the leading "/" and all consumed
    /// tokens so that callers only need to append each match.
    ///
    /// \param[in] buf The current input buffer.
    /// \return Completion candidates and the prefix to prepend.
    CompletionResult compute_completions(const std::string& buf) const;

    /// \brief Walk the command tree to compute an inline hint for \p buf.
    ///
    /// Finds the first non-parameter child whose keyword starts with
    /// the partial token at the cursor. The returned text is the
    /// remainder of the keyword after the already-typed prefix.
    ///
    /// \param[in] buf The current input buffer.
    /// \return The hint remainder text and active flag.
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
