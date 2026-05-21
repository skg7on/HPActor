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

#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Command class definitions follow the same pattern as the production commands
// (see src/cli/commands/): enclosed in hpactor::cli::(anonymous) so that
// names like ICommand and result<void> are visible through namespace nesting.
// ---------------------------------------------------------------------------

namespace hpactor {
namespace cli {
namespace {

// =============================================================================
// Test commands used in add/retrieve and mount_command tests
// =============================================================================

class TestAddCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "test/add/path";
    }
    std::string_view help_text() const noexcept override {
        return "Test add command";
    }
    int order() const noexcept override {
        return 99999;
    }
    result<void> execute(CommandContext& /*ctx*/) const override {
        return result<void>::make();
    }
};

class TestStaticCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "test/static/registration";
    }
    std::string_view help_text() const noexcept override {
        return "Static registration test";
    }
    int order() const noexcept override {
        return 100000;
    }
    result<void> execute(CommandContext& /*ctx*/) const override {
        return result<void>::make();
    }
};

// Static registration — fires during static init, before main().
const CommandRegistration<TestStaticCommand> kRegisterTestStatic;

class SimpleCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "simple";
    }
    std::string_view help_text() const noexcept override {
        return "Simple command";
    }
    int order() const noexcept override {
        return 0;
    }
    result<void> execute(CommandContext& /*ctx*/) const override {
        return result<void>::make();
    }
};

class ParameterizedCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show actor";
    }
    int order() const noexcept override {
        return 1;
    }
    result<void> execute(CommandContext& /*ctx*/) const override {
        return result<void>::make();
    }
};

class DrainCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/drain";
    }
    std::string_view help_text() const noexcept override {
        return "Drain system";
    }
    int order() const noexcept override {
        return 2;
    }
    result<void> execute(CommandContext& /*ctx*/) const override {
        return result<void>::make();
    }
};

class DrainStatusCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "system/drain/status";
    }
    std::string_view help_text() const noexcept override {
        return "Drain status";
    }
    int order() const noexcept override {
        return 3;
    }
    result<void> execute(CommandContext& /*ctx*/) const override {
        return result<void>::make();
    }
};

} // anonymous namespace
} // namespace cli
} // namespace hpactor

// ---------------------------------------------------------------------------
// Test functions — at global scope, pull in hpactor::cli with using-directive.
// ---------------------------------------------------------------------------

using namespace hpactor::cli;

// mount_command is defined inside namespace hpactor::cli in cli_actor.cpp
namespace hpactor {
namespace cli {
extern void mount_command(CommandNode* root, const ICommand& cmd);
}
} // namespace hpactor

// =============================================================================
// parse_command_path tests
// =============================================================================

void test_parse_simple_path() {
    auto segs = parse_command_path("help");
    assert(segs.size() == 1);
    assert(segs[0] == "help");
}

void test_parse_multi_segment_path() {
    auto segs = parse_command_path("actor/list");
    assert(segs.size() == 2);
    assert(segs[0] == "actor");
    assert(segs[1] == "list");
}

void test_parse_param_path() {
    auto segs = parse_command_path("actor/<id>/show");
    assert(segs.size() == 3);
    assert(segs[0] == "actor");
    assert(segs[1] == "<id>");
    assert(segs[2] == "show");
}

void test_parse_empty_path() {
    auto segs = parse_command_path("");
    assert(segs.empty());
}

void test_parse_leading_trailing_slashes() {
    auto segs = parse_command_path("//actor//list/");
    // Consecutive slashes produce empty segments that are skipped
    assert(segs.size() == 2);
    assert(segs[0] == "actor");
    assert(segs[1] == "list");
}

// =============================================================================
// is_param_segment tests
// =============================================================================

void test_is_param_valid() {
    assert(is_param_segment("<id>"));
    assert(is_param_segment("<actor_id>"));
    assert(is_param_segment("<filter>"));
}

void test_is_param_literal() {
    assert(!is_param_segment("actor"));
    assert(!is_param_segment("show"));
    assert(!is_param_segment(""));
}

void test_is_param_edge_cases() {
    // "<>" — technically starts and ends with brackets, treated as param
    assert(is_param_segment("<>"));
    assert(!is_param_segment("<"));   // opening bracket only
    assert(!is_param_segment(">"));   // closing bracket only
    assert(!is_param_segment("<id")); // missing closing bracket
    assert(!is_param_segment("id>")); // missing opening bracket
}

// =============================================================================
// Registry singleton tests
// =============================================================================

void test_registry_singleton() {
    auto& r1 = CommandRegistry::instance();
    auto& r2 = CommandRegistry::instance();
    assert(&r1 == &r2);
}

// =============================================================================
// Registry add and retrieve tests
// =============================================================================

void test_add_and_retrieve() {
    auto& registry = CommandRegistry::instance();
    size_t initial_count = registry.commands().size();

    registry.add(std::make_unique<TestAddCommand>());

    assert(registry.commands().size() == initial_count + 1);
    auto& last = *registry.commands().back();
    assert(last.path() == "test/add/path");
    assert(last.help_text() == "Test add command");
    assert(last.order() == 99999);
}

// =============================================================================
// Static registration test
//
// TestStaticCommand is registered via CommandRegistration<T> at file scope
// inside hpactor::cli::(anonymous). This fires during static initialization,
// before main(). The test verifies the command can be found in the registry.
// =============================================================================

void test_static_registration() {
    auto& registry = CommandRegistry::instance();
    bool found = false;
    for (auto& cmd : registry.commands()) {
        if (cmd->path() == "test/static/registration") {
            found = true;
            assert(cmd->help_text() == "Static registration test");
            assert(cmd->order() == 100000);
            break;
        }
    }
    assert(found && "Static-registered command not found in registry");
}

// =============================================================================
// mount_command tests
// =============================================================================

void test_mount_simple_command() {
    CommandNode root{"", "root"};
    SimpleCommand cmd;
    mount_command(&root, cmd);

    assert(root.children.size() == 1);
    assert(root.children[0]->keyword == "simple");
    assert(root.children[0]->help_text == "Simple command");
    assert(!root.children[0]->is_parameter);
    assert(root.children[0]->execute != nullptr);
    assert(root.children[0]->children.empty());
}

void test_mount_parameterized_command() {
    CommandNode root{"", "root"};
    ParameterizedCommand cmd;
    mount_command(&root, cmd);

    // root -> actor -> <id> -> show
    assert(root.children.size() == 1);
    assert(root.children[0]->keyword == "actor");
    assert(!root.children[0]->is_parameter);

    auto* actor = root.children[0].get();
    assert(actor->children.size() == 1);
    assert(actor->children[0]->keyword == "<id>");
    assert(actor->children[0]->is_parameter);

    auto* id_node = actor->children[0].get();
    assert(id_node->children.size() == 1);
    assert(id_node->children[0]->keyword == "show");
    assert(id_node->children[0]->help_text == "Show actor");
    assert(id_node->children[0]->execute != nullptr);
    assert(!id_node->children[0]->is_parameter);
}

void test_mount_shared_prefix() {
    CommandNode root{"", "root"};

    // Mount "system/drain" first
    DrainCommand drain_cmd;
    mount_command(&root, drain_cmd);

    assert(root.children.size() == 1);
    assert(root.children[0]->keyword == "system");

    auto* system_node = root.children[0].get();
    assert(system_node->children.size() == 1);
    assert(system_node->children[0]->keyword == "drain");

    auto* drain_node = system_node->children[0].get();
    assert(drain_node->execute != nullptr);
    assert(drain_node->help_text == "Drain system");

    // Now mount "system/drain/status" — shares "system/drain" prefix
    DrainStatusCommand status_cmd;
    mount_command(&root, status_cmd);

    // Root still has one child (system)
    assert(root.children.size() == 1);
    assert(root.children[0]->keyword == "system");

    // "system" still has one child (drain)
    assert(system_node->children.size() == 1);
    assert(system_node->children[0]->keyword == "drain");

    // "drain" node still has execute from first mount
    assert(drain_node->execute != nullptr);
    assert(drain_node->help_text == "Drain system");

    // "drain" now has a child "status" from second mount
    assert(drain_node->children.size() == 1);
    assert(drain_node->children[0]->keyword == "status");
    assert(drain_node->children[0]->help_text == "Drain status");
    assert(drain_node->children[0]->execute != nullptr);
}

// =============================================================================
// main
// =============================================================================

int main() {
    // parse_command_path tests
    test_parse_simple_path();
    test_parse_multi_segment_path();
    test_parse_param_path();
    test_parse_empty_path();
    test_parse_leading_trailing_slashes();
    printf("  parse_command_path: OK\n");

    // is_param_segment tests
    test_is_param_valid();
    test_is_param_literal();
    test_is_param_edge_cases();
    printf("  is_param_segment: OK\n");

    // Registry tests
    test_registry_singleton();
    test_add_and_retrieve();
    test_static_registration();
    printf("  registry: OK\n");

    // mount_command tests
    test_mount_simple_command();
    test_mount_parameterized_command();
    test_mount_shared_prefix();
    printf("  mount_command: OK\n");

    printf("test_command_registry: PASSED\n");
    return 0;
}
