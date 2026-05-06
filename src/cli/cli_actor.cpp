// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/lexer.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cstdio>
#include <iostream>

namespace hpactor {
namespace cli {

CliActor::CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config)
    : DaemonActor(ctx, system)
    , system_(system)
    , config_(config)
{
    formatter_ = OutputFormatter::create(config.default_format);
    pager_ = std::make_unique<Pager>(config.page_size);
    build_command_tree();
}

void CliActor::on_daemon_start() {
    print_greeting();
}

void CliActor::on_daemon_stop() {
    printf("\n[CLI session ended]\n");
}

void CliActor::print_greeting() {
    printf("HPActor CLI v1.0 — Type /help for available commands. /quit to exit.\n\n");
}

void CliActor::print_prompt() {
    printf("hpactor> ");
    fflush(stdout);
}

void CliActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");

    // /actor <id> ...
    auto* actor = root->add_child("actor", "Actor operations");
    auto* actor_id = actor->add_child("<id>", "Target actor ID", /*is_param=*/true);

    actor_id->add_child("show", "Display actor metadata, state, mailbox, and children")
        ->execute = [](CommandContext& ctx) -> result<void> {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> show)");
            return result<void>::make();
        }
        ctx.output->header("Actor " + *id_str);
        ctx.output->key_value({{"Status", "pending..."}});
        return result<void>::make();
    };

    actor_id->add_child("kill", "Terminate actor")
        ->execute = [](CommandContext& ctx) -> result<void> {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID");
            return result<void>::make();
        }
        ctx.output->raw("kill " + *id_str + " — not yet implemented");
        return result<void>::make();
    };

    // /actor list
    actor->add_child("list", "List all actors")
        ->execute = [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Actor List");
        ctx.output->raw("list — not yet implemented");
        return result<void>::make();
    };

    // /system ...
    auto* sys = root->add_child("system", "System operations");
    sys->add_child("stats", "System statistics")
        ->execute = [](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Statistics");
        ctx.output->raw("stats — not yet implemented");
        return result<void>::make();
    };
    sys->add_child("memory", "Memory subsystem stats")
        ->execute = [](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Memory");
        ctx.output->raw("memory — not yet implemented");
        return result<void>::make();
    };
    sys->add_child("list", "List system actors")
        ->execute = [](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Actors");
        ctx.output->raw("system list — not yet implemented");
        return result<void>::make();
    };

    // /metrics ...
    auto* metrics = root->add_child("metrics", "Metrics operations");
    metrics->add_child("show", "Show current metrics snapshot")
        ->execute = [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Metrics");
        ctx.output->raw("metrics show — not yet implemented");
        return result<void>::make();
    };

    // /topology ...
    auto* topo = root->add_child("topology", "Topology operations");
    topo->add_child("show", "Show topology tree")
        ->execute = [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Topology");
        ctx.output->raw("topology show — not yet implemented");
        return result<void>::make();
    };

    // /help
    root->add_child("help", "Show available commands")
        ->execute = [this](CommandContext& ctx) -> result<void> {
        ctx.output->header("Available Commands");
        ctx.output->raw(command_tree_->help());
        return result<void>::make();
    };

    // /quit
    root->add_child("quit", "Exit the CLI")
        ->execute = [this](CommandContext& ctx) -> result<void> {
        ctx.output->raw("Goodbye.");
        running_ = false;
        return result<void>::make();
    };

    command_tree_ = std::move(root);
}

void CliActor::execute_tokens(const std::vector<Token>& tokens) {
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
    if (!running_) {
        return false;
    }

    print_prompt();

    std::string line;
    if (!std::getline(std::cin, line)) {
        printf("\nGoodbye.\n");
        return false;
    }

    if (line.empty()) {
        return true;
    }

    auto tokens = Lexer::tokenize(line);
    execute_tokens(tokens);
    return true;
}

}  // namespace cli
}  // namespace hpactor
