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

#include <gtest/gtest.h>
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

TEST(CommandRegistryTest, ParseSimplePath) {
    auto segs = parse_command_path("help");
    EXPECT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0], "help");
}

TEST(CommandRegistryTest, ParseMultiSegmentPath) {
    auto segs = parse_command_path("actor/list");
    EXPECT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0], "actor");
    EXPECT_EQ(segs[1], "list");
}

TEST(CommandRegistryTest, ParseParamPath) {
    auto segs = parse_command_path("actor/<id>/show");
    EXPECT_EQ(segs.size(), 3u);
    EXPECT_EQ(segs[0], "actor");
    EXPECT_EQ(segs[1], "<id>");
    EXPECT_EQ(segs[2], "show");
}

TEST(CommandRegistryTest, ParseEmptyPath) {
    auto segs = parse_command_path("");
    EXPECT_TRUE(segs.empty());
}

TEST(CommandRegistryTest, ParseLeadingTrailingSlashes) {
    auto segs = parse_command_path("//actor//list/");
    // Consecutive slashes produce empty segments that are skipped
    EXPECT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0], "actor");
    EXPECT_EQ(segs[1], "list");
}

// =============================================================================
// is_param_segment tests
// =============================================================================

TEST(CommandRegistryTest, IsParamValid) {
    EXPECT_TRUE(is_param_segment("<id>"));
    EXPECT_TRUE(is_param_segment("<actor_id>"));
    EXPECT_TRUE(is_param_segment("<filter>"));
}

TEST(CommandRegistryTest, IsParamLiteral) {
    EXPECT_FALSE(is_param_segment("actor"));
    EXPECT_FALSE(is_param_segment("show"));
    EXPECT_FALSE(is_param_segment(""));
}

TEST(CommandRegistryTest, IsParamEdgeCases) {
    // "<>" — technically starts and ends with brackets, treated as param
    EXPECT_TRUE(is_param_segment("<>"));
    EXPECT_FALSE(is_param_segment("<"));   // opening bracket only
    EXPECT_FALSE(is_param_segment(">"));   // closing bracket only
    EXPECT_FALSE(is_param_segment("<id")); // missing closing bracket
    EXPECT_FALSE(is_param_segment("id>")); // missing opening bracket
}

// =============================================================================
// Registry singleton tests
// =============================================================================

TEST(CommandRegistryTest, RegistrySingleton) {
    auto& r1 = CommandRegistry::instance();
    auto& r2 = CommandRegistry::instance();
    EXPECT_EQ(&r1, &r2);
}

// =============================================================================
// Registry add and retrieve tests
// =============================================================================

TEST(CommandRegistryTest, AddAndRetrieve) {
    auto& registry = CommandRegistry::instance();
    size_t initial_count = registry.commands().size();

    registry.add(std::make_unique<TestAddCommand>());

    EXPECT_EQ(registry.commands().size(), initial_count + 1);
    auto& last = *registry.commands().back();
    EXPECT_EQ(last.path(), "test/add/path");
    EXPECT_EQ(last.help_text(), "Test add command");
    EXPECT_EQ(last.order(), 99999);
}

// =============================================================================
// Static registration test
//
// TestStaticCommand is registered via CommandRegistration<T> at file scope
// inside hpactor::cli::(anonymous). This fires during static initialization,
// before main(). The test verifies the command can be found in the registry.
// =============================================================================

TEST(CommandRegistryTest, StaticRegistration) {
    auto& registry = CommandRegistry::instance();
    bool found = false;
    for (auto& cmd : registry.commands()) {
        if (cmd->path() == "test/static/registration") {
            found = true;
            EXPECT_EQ(cmd->help_text(), "Static registration test");
            EXPECT_EQ(cmd->order(), 100000);
            break;
        }
    }
    EXPECT_TRUE(found) << "Static-registered command not found in registry";
}

// =============================================================================
// mount_command tests
// =============================================================================

TEST(CommandRegistryTest, MountSimpleCommand) {
    CommandNode root{"", "root"};
    SimpleCommand cmd;
    mount_command(&root, cmd);

    EXPECT_EQ(root.children.size(), 1u);
    EXPECT_EQ(root.children[0]->keyword, "simple");
    EXPECT_EQ(root.children[0]->help_text, "Simple command");
    EXPECT_FALSE(root.children[0]->is_parameter);
    EXPECT_NE(root.children[0]->execute, nullptr);
    EXPECT_TRUE(root.children[0]->children.empty());
}

TEST(CommandRegistryTest, MountParameterizedCommand) {
    CommandNode root{"", "root"};
    ParameterizedCommand cmd;
    mount_command(&root, cmd);

    // root -> actor -> <id> -> show
    EXPECT_EQ(root.children.size(), 1u);
    EXPECT_EQ(root.children[0]->keyword, "actor");
    EXPECT_FALSE(root.children[0]->is_parameter);

    auto* actor = root.children[0].get();
    EXPECT_EQ(actor->children.size(), 1u);
    EXPECT_EQ(actor->children[0]->keyword, "<id>");
    EXPECT_TRUE(actor->children[0]->is_parameter);

    auto* id_node = actor->children[0].get();
    EXPECT_EQ(id_node->children.size(), 1u);
    EXPECT_EQ(id_node->children[0]->keyword, "show");
    EXPECT_EQ(id_node->children[0]->help_text, "Show actor");
    EXPECT_NE(id_node->children[0]->execute, nullptr);
    EXPECT_FALSE(id_node->children[0]->is_parameter);
}

TEST(CommandRegistryTest, MountSharedPrefix) {
    CommandNode root{"", "root"};

    // Mount "system/drain" first
    DrainCommand drain_cmd;
    mount_command(&root, drain_cmd);

    EXPECT_EQ(root.children.size(), 1u);
    EXPECT_EQ(root.children[0]->keyword, "system");

    auto* system_node = root.children[0].get();
    EXPECT_EQ(system_node->children.size(), 1u);
    EXPECT_EQ(system_node->children[0]->keyword, "drain");

    auto* drain_node = system_node->children[0].get();
    EXPECT_NE(drain_node->execute, nullptr);
    EXPECT_EQ(drain_node->help_text, "Drain system");

    // Now mount "system/drain/status" — shares "system/drain" prefix
    DrainStatusCommand status_cmd;
    mount_command(&root, status_cmd);

    // Root still has one child (system)
    EXPECT_EQ(root.children.size(), 1u);
    EXPECT_EQ(root.children[0]->keyword, "system");

    // "system" still has one child (drain)
    EXPECT_EQ(system_node->children.size(), 1u);
    EXPECT_EQ(system_node->children[0]->keyword, "drain");

    // "drain" node still has execute from first mount
    EXPECT_NE(drain_node->execute, nullptr);
    EXPECT_EQ(drain_node->help_text, "Drain system");

    // "drain" now has a child "status" from second mount
    EXPECT_EQ(drain_node->children.size(), 1u);
    EXPECT_EQ(drain_node->children[0]->keyword, "status");
    EXPECT_EQ(drain_node->children[0]->help_text, "Drain status");
    EXPECT_NE(drain_node->children[0]->execute, nullptr);
}
