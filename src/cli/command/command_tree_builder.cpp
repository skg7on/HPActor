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

#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/command/command_tree_builder.hpp>

#include <algorithm>
#include <vector>

namespace hpactor {
namespace cli {

void mount_command(CommandNode* root, const ICommand& cmd) {
    auto segments = parse_command_path(cmd.path());
    if (segments.empty())
        return;

    CommandNode* node = root;
    for (size_t i = 0; i < segments.size(); ++i) {
        auto& seg = segments[i];
        bool is_param = is_param_segment(seg);
        bool is_last = (i == segments.size() - 1);

        // Find existing child or create one
        CommandNode* child = nullptr;
        for (auto& c : node->children) {
            if (c->keyword == seg) {
                child = c.get();
                break;
            }
        }
        if (!child) {
            child = node->add_child(seg, "", is_param);
        }

        if (is_last) {
            child->help_text = cmd.help_text();
            child->execute = [&cmd](CommandContext& ctx) -> result<void> {
                return cmd.execute(ctx);
            };
        }
        node = child;
    }
}

void build_command_tree_from_registry(CommandNode& root) {
    auto& cmds = CommandRegistry::instance().commands();
    // Sort by order for deterministic tree assembly
    std::vector<const ICommand*> sorted;
    sorted.reserve(cmds.size());
    for (auto& c : cmds)
        sorted.push_back(c.get());
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order)
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const ICommand* a, const ICommand* b) {
                         if (a->order() != b->order())
                             return a->order() < b->order();
                         return a->path() < b->path();
                     });

    for (auto* cmd : sorted) {
        mount_command(&root, *cmd);
    }
}

} // namespace cli
} // namespace hpactor
