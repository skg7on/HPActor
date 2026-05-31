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

#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/line_editor.hpp>
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
    if (line == nullptr)
        return {};
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
    if (callbacks_installed_)
        return;
    linenoiseSetCompletionCallback(on_completion);
    linenoiseSetHintsCallback(on_hints);
    linenoiseSetFreeHintsCallback(on_free_hints);
    callbacks_installed_ = true;
}

std::vector<std::string> LineEditor::tokenize_partial(const std::string& buf) {
    auto tokens = Lexer::tokenize(buf);
    std::vector<std::string> words;
    for (auto& t : tokens) {
        if (t.type == TokenType::Eof)
            continue;
        words.push_back(std::move(t.value));
    }
    return words;
}

CompletionResult LineEditor::compute_completions(const std::string& buf) const {
    CompletionResult result;
    if (!root_ || buf.empty())
        return result;

    auto words = tokenize_partial(buf);
    bool ends_with_space = (!buf.empty() && buf.back() == ' ');

    const CommandNode* node = root_;
    std::vector<std::string> consumed;
    size_t i = 0;
    if (i < words.size() && words[i] == "/")
        ++i;

    size_t words_to_consume =
        ends_with_space ? words.size() : (words.size() > 0 ? words.size() - 1 : 0);
    for (; i < words_to_consume; ++i) {
        std::string param;
        auto* child = node->find_child(words[i], param);
        if (!child)
            child = node->find_child_prefix(words[i]);
        if (!child) {
            node->collect_completions(words[i], result.matches);
            result.prefix = "/";
            for (auto& w : consumed)
                result.prefix += w + " ";
            return result;
        }
        consumed.push_back(child->is_parameter ? words[i] : child->keyword);
        node = child;
    }

    std::string partial;
    if (!ends_with_space && !words.empty())
        partial = words.back();

    if (!partial.empty()) {
        for (auto& child : node->children) {
            if (!child->is_parameter && child->keyword == partial) {
                consumed.push_back(partial);
                node = child.get();
                partial.clear();
                break;
            }
        }
    }

    result.prefix = "/";
    for (auto& w : consumed)
        result.prefix += w + " ";

    node->collect_completions(partial, result.matches);
    return result;
}

HintResult LineEditor::compute_hint(const std::string& buf) const {
    HintResult result;
    if (!root_)
        return result;

    auto words = tokenize_partial(buf);
    if (buf.empty())
        return result;
    bool ends_with_space = (buf.back() == ' ');

    const CommandNode* node = root_;
    size_t i = 0;
    if (i < words.size() && words[i] == "/")
        ++i;

    size_t words_to_consume =
        ends_with_space ? words.size() : (words.size() > 0 ? words.size() - 1 : 0);
    for (; i < words_to_consume; ++i) {
        std::string param;
        auto* child = node->find_child(words[i], param);
        if (!child) {
            child = node->find_child_prefix(words[i]);
            if (!child)
                return result;
        }
        node = child;
    }

    std::string partial;
    if (!ends_with_space && !words.empty()) {
        partial = words.back();
    }

    if (!partial.empty()) {
        for (auto& child : node->children) {
            if (!child->is_parameter && child->keyword == partial) {
                node = child.get();
                partial.clear();
                break;
            }
        }
    }

    for (auto& child : node->children) {
        if (child->is_parameter)
            continue;
        if (partial.empty() || child->keyword.starts_with(partial)) {
            result.text = child->keyword.substr(partial.size());
            result.active = true;
            return result;
        }
    }
    return result;
}

void LineEditor::on_completion(const char* buf, linenoiseCompletions* lc) {
    auto* self = current_;
    if (!self || !self->root_)
        return;

    auto result = self->compute_completions(buf);
    for (auto& m : result.matches)
        linenoiseAddCompletion(lc, (result.prefix + m).c_str());
}

char* LineEditor::on_hints(const char* buf, int* color, int* bold) {
    auto* self = current_;
    if (!self || !self->root_)
        return nullptr;

    auto hint = self->compute_hint(buf);
    if (!hint.active)
        return nullptr;
    if (color)
        *color = 90;
    if (bold)
        *bold = 0;
    return strdup(hint.text.c_str());
}

void LineEditor::on_free_hints(void* hint) {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    free(hint); // C API callback, must free strdup result
}

} // namespace hpactor::cli