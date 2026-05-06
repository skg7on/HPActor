// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward-declare linenoise C types to avoid pulling linenoise.h into public headers.
struct linenoiseCompletions;

namespace hpactor::cli {

struct CommandNode;

struct LineEditorConfig {
    std::string history_path;
    uint32_t history_max = 1000;
    bool multiline = false;
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
    void set_root(const CommandNode* root) { root_ = root; }

private:
    // linenoise completion callback (global, no ctx — uses current_ editor pointer).
    // Called on Tab to populate completions.
    static void on_completion(const char* buf,
                              struct linenoiseCompletions* lc);

    // linenoise hints callback (global, no ctx — uses current_ editor pointer).
    // Called on each keystroke; returns gray hint string.
    static char* on_hints(const char* buf,
                          int* color,
                          int* bold);

    // linenoise free-hints callback (global). Frees the string returned by on_hints.
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
