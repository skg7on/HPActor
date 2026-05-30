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

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace hpactor {
namespace cli {

std::string CliActor::get_history_path(const CliConfig& config) {
    if (!config.history_path.empty())
        return config.history_path;
    const char* home = getenv("HOME");
    if (!home)
        home = "/tmp";
    return std::string(home) + "/.hpactor_history";
}

CliActor::CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      line_editor_(LineEditorConfig{get_history_path(config), config.history_max,
                                    /*multiline=*/false},
                   /*root=*/nullptr) {
    formatter_ = OutputFormatter::create(config.default_format);
    pager_ = std::make_unique<Pager>(config.page_size);
    build_command_tree();
    line_editor_.set_root(command_tree_.get());
    line_editor_.load_history();
}

void CliActor::on_daemon_start() {
    print_greeting();
}

void CliActor::on_daemon_stop() {
    line_editor_.save_history();
    printf("\n[CLI session ended]\n");
}

void CliActor::print_greeting() {
    printf("HPActor CLI v1.0 — Type /help for available commands. /quit to "
           "exit.\n\n");
}

// ---------------------------------------------------------------------------
// Mailbox polling — block on this dedicated thread until the expected
// response tag arrives or the timeout expires.
// ---------------------------------------------------------------------------

std::optional<StreamBuffer>
CliActor::poll_for_response(TypeTag expected_tag, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage msg;
        if (mailbox()->try_pop(msg)) {
            if (msg.type_id() == expected_tag) {
                return std::move(msg).payload();
            }
            // Discard unexpected messages. CliActor is a system actor —
            // no other actor links to or monitors it, so the only expected
            // traffic is replies to its own requests.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// send_and_wait helpers
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliActor::send_and_wait_inspect(ActorId target, const InspectStateRequest& req,
                                std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;

    TypedMessage msg(TypeTag::InspectStateRequestTag, req);
    context()->send(actor->address(), std::move(msg));

    auto payload = poll_for_response(TypeTag::InspectStateResponseTag, timeout);
    if (!payload)
        return std::nullopt;

    InspectStateReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
        return std::nullopt;
    }
    return reply;
}

std::optional<KillReply>
CliActor::send_and_wait_kill(ActorId target, const KillRequest& req,
                             std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;

    TypedMessage msg(TypeTag::KillRequestTag, req);
    context()->send(actor->address(), std::move(msg));

    auto payload = poll_for_response(TypeTag::KillResponseTag, timeout);
    if (!payload)
        return std::nullopt;

    KillReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
        return std::nullopt;
    }
    return reply;
}

// ---------------------------------------------------------------------------
// Actor enumeration — iterates the system actor map under lock.
// ---------------------------------------------------------------------------

std::vector<ActorMeta> CliActor::enumerate_actors(const std::string& filter) {
    std::vector<ActorMeta> result;
    system_.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (!filter.empty()) {
            std::string type_name(actor.type_name().data(),
                                  actor.type_name().size());
            if (type_name.find(filter) == std::string::npos)
                return;
        }
        auto meta = actor.to_metadata();
        result.push_back(std::move(meta));
    });
    return result;
}

// ---------------------------------------------------------------------------
// Command tree — registered commands wired to real implementations.
// ---------------------------------------------------------------------------

// Mount a single registered command into the tree, creating intermediate
// nodes as needed. Sets execute on the terminal node.
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

void CliActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");

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
        mount_command(root.get(), *cmd);
    }

    command_tree_ = std::move(root);
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

void CliActor::execute_tokens(const std::vector<Token>& tokens) {
    FAULT_INJECT("hpactor.cli.execute_tokens.corrupt") {
        return;  // silently skip command execution
    }
    // Reopen formatter for each command
    formatter_ = OutputFormatter::create(config_.default_format);

    CommandContext ctx;
    ctx.system = &system_;
    ctx.cli_actor = this;
    ctx.output = formatter_.get();
    ctx.page_size = config_.page_size;

    // Walk the command tree
    CommandNode* node = command_tree_.get();

    size_t i = 0;
    // Skip leading "/" keyword
    if (i < tokens.size() && tokens[i].value == "/") {
        ++i;
    }

    for (; i < tokens.size(); ++i) {
        auto& tok = tokens[i];

        if (tok.type == TokenType::Eof) {
            break;
        }

        if (tok.type == TokenType::Flag) {
            ctx.params[tok.value] = "true";
            continue;
        }

        if (tok.type == TokenType::FlagWithArg) {
            ctx.params[tok.value] = tok.arg.value_or("true");
            if (tok.value == "format") {
                ctx.format = tok.arg.value_or("pretty");
                formatter_ = OutputFormatter::create(ctx.format);
                ctx.output = formatter_.get();
            }
            continue;
        }

        // Try to match as keyword or parameter
        std::string param_value;
        auto* child = node->find_child(tok.value, param_value);
        if (!child) {
            auto suggestion = node->suggest(tok.value);
            std::string err = "Unknown command '" + tok.value + "'";
            if (!suggestion.empty()) {
                err += " — did you mean '" + suggestion + "'?";
            }
            formatter_->error(err);
            printf("%s\n", formatter_->finalize().c_str());
            return;
        }

        if (child->is_parameter) {
            ctx.params[child->keyword] = param_value;
        }
        node = child;
    }

    // Execute leaf node
    if (node->execute) {
        node->execute(ctx);
    } else {
        // No execute — show help for this node
        if (!node->children.empty()) {
            formatter_->header("Available commands");
            formatter_->raw(node->help());
        }
    }

    printf("%s\n", formatter_->finalize().c_str());
}

bool CliActor::run_once() {
    FAULT_INJECT("hpactor.cli.actor.run_once.fail") {
        return false;  // daemon exits
    }
    if (!running_) {
        return false;
    }

    std::string line = line_editor_.readline("hpactor> ");
    if (line.empty()) {
        printf("\nGoodbye.\n");
        running_ = false;
        return false;
    }

    auto tokens = Lexer::tokenize(line);
    execute_tokens(tokens);
    line_editor_.add_history(line);
    return true;
}

} // namespace cli
} // namespace hpactor