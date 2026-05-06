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

// Static singleton pointer — CliActor is the sole LineEditor user.
LineEditor* LineEditor::current_ = nullptr;

LineEditor::LineEditor(const LineEditorConfig& cfg, const CommandNode* root)
    : root_(root), config_(cfg) {
    current_ = this;
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
    current_ = nullptr;
}

std::string LineEditor::readline(const std::string& prompt) {
    install_callbacks();
    char* line = linenoise(prompt.c_str());
    if (line == nullptr) return {};
    std::string result(line);
    linenoiseFree(line);
    return result;
}

void LineEditor::add_history(const std::string& line) const {
    linenoiseHistoryAdd(line.c_str());
    if (!config_.history_path.empty()) {
        linenoiseHistorySave(config_.history_path.c_str());
    }
}

void LineEditor::load_history() const {
    if (!config_.history_path.empty()) {
        linenoiseHistoryLoad(config_.history_path.c_str());
    }
}

void LineEditor::save_history() const {
    if (!config_.history_path.empty()) {
        linenoiseHistorySave(config_.history_path.c_str());
    }
}

void LineEditor::install_callbacks() {
    if (callbacks_installed_) return;
    linenoiseSetCompletionCallback(on_completion);
    linenoiseSetHintsCallback(on_hints);
    linenoiseSetFreeHintsCallback(on_free_hints);
    callbacks_installed_ = true;
}

std::vector<std::string> LineEditor::tokenize_partial(const std::string& buf) {
    auto tokens = Lexer::tokenize(buf);
    std::vector<std::string> words;
    for (auto& t : tokens) {
        if (t.type == TokenType::Eof) continue;
        words.push_back(std::move(t.value));
    }
    return words;
}

void LineEditor::on_completion(const char* buf,
                               linenoiseCompletions* lc) {
    auto* self = current_;
    if (!self || !self->root_) return;

    auto words = tokenize_partial(buf);
    size_t len = strlen(buf);
    bool ends_with_space = (len > 0 && buf[len - 1] == ' ');

    const CommandNode* node = self->root_;
    size_t i = 0;
    // Skip leading "/"
    if (i < words.size() && words[i] == "/") ++i;

    // Consume fully-typed tokens (all but the last when not space-terminated)
    size_t words_to_consume = ends_with_space ? words.size() : (words.size() > 0 ? words.size() - 1 : 0);
    for (; i < words_to_consume; ++i) {
        std::string param;
        auto* child = node->find_child(words[i], param);
        if (!child) return;  // unknown token, can't complete
        node = child;
    }

    // Determine partial token to complete
    std::string partial;
    if (!ends_with_space && !words.empty()) {
        partial = words.back();
    }

    // When the partial is an exact keyword match, offer "keyword " so
    // linenoise adds a space instead of replacing the keyword with a
    // child completion. On the next Tab, the user gets sub-commands.
    bool exact_match_found = false;
    if (!partial.empty()) {
        for (auto& child : node->children) {
            if (!child->is_parameter && child->keyword == partial) {
                std::string with_space = child->keyword + " ";
                linenoiseAddCompletion(lc, with_space.c_str());
                node = child.get();
                partial.clear();
                exact_match_found = true;
                break;
            }
        }
    }

    // Collect matching non-parameter children.
    // Skip if we already handled an exact match (the space-completion is
    // sufficient; children appear on the next Tab after the space).
    if (!exact_match_found) {
        for (auto& child : node->children) {
            if (child->is_parameter) continue;
            if (partial.empty() || child->keyword.starts_with(partial)) {
                linenoiseAddCompletion(lc, child->keyword.c_str());
            }
        }
    }
}

char* LineEditor::on_hints(const char* buf,
                           int* color,
                           int* bold) {
    auto* self = current_;
    if (!self || !self->root_) return nullptr;

    auto words = tokenize_partial(buf);
    size_t len = strlen(buf);
    if (len == 0) return nullptr;
    bool ends_with_space = (buf[len - 1] == ' ');

    const CommandNode* node = self->root_;
    size_t i = 0;
    // Skip leading "/"
    if (i < words.size() && words[i] == "/") ++i;

    // Consume fully-typed tokens
    size_t words_to_consume = ends_with_space ? words.size() : (words.size() > 0 ? words.size() - 1 : 0);
    for (; i < words_to_consume; ++i) {
        std::string param;
        auto* child = node->find_child(words[i], param);
        if (!child) return nullptr;  // unknown token, no hint possible
        node = child;
    }

    // Determine partial token
    std::string partial;
    if (!ends_with_space && !words.empty()) {
        partial = words.back();
    }

    // If the partial exactly matches a child keyword, the user typed a
    // complete token — advance to show hints from the next level.
    if (!partial.empty()) {
        for (auto& child : node->children) {
            if (!child->is_parameter && child->keyword == partial) {
                node = child.get();
                partial.clear();
                break;
            }
        }
    }

    // Find first non-parameter child whose keyword starts with partial
    for (auto& child : node->children) {
        if (child->is_parameter) continue;
        if (partial.empty() || child->keyword.starts_with(partial)) {
            std::string hint_text = child->keyword.substr(partial.size());
            if (color) *color = 90;   // bright black = gray
            if (bold) *bold = 0;
            return strdup(hint_text.c_str());
        }
    }
    return nullptr;
}

void LineEditor::on_free_hints(void* hint) {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) — C API callback, must free strdup result
    free(hint);
}

} // namespace hpactor::cli
