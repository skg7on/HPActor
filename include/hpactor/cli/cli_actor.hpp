// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/token.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliActor : public DaemonActor {
public:
    CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config);

    // DaemonActor interface
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;

    // Accessors for commands
    ActorSystem& system() { return system_; }
    const CliConfig& config() const { return config_; }
    OutputFormatter* formatter() { return formatter_.get(); }
    Pager* pager() { return pager_.get(); }

private:
    void build_command_tree();
    void execute_tokens(const std::vector<Token>& tokens);
    void print_prompt();
    void print_greeting();

    ActorSystem& system_;
    CliConfig config_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    bool running_ = true;
};

}  // namespace cli
}  // namespace hpactor
