// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/token.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

// Forward-declare protobuf types (defined in cli_messages.pb.h)
class InspectStateReply;
class KillReply;
class ListActorsReply;
class SystemStatsReply;
class MemoryStatsReply;

class CliActor : public DaemonActor {
  public:
    CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config);

    // DaemonActor interface
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;

    bool is_system_actor() const override {
        return true;
    }

    // Accessors for commands
    ActorSystem& system() {
        return system_;
    }
    const CliConfig& config() const {
        return config_;
    }
    OutputFormatter* formatter() {
        return formatter_.get();
    }
    Pager* pager() {
        return pager_.get();
    }

    // Whether the CLI input loop is still running.
    // Set to false by /quit or EOF on stdin.
    bool is_running() const {
        return running_;
    }

    // --- Request-Response Helpers ---
    //
    // Send an InspectStateRequest to target and block on the reply.
    // Polls this actor's mailbox on the dedicated thread — safe, no
    // scheduler contention since CliActor uses DispatchPolicy::DedicatedThread.
    std::optional<InspectStateReply> send_and_wait_inspect(
        ActorId target, const class InspectStateRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    std::optional<KillReply> send_and_wait_kill(
        ActorId target, const class KillRequest& req,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    // Enumerate all known actors. Returns metadata for each.
    std::vector<ActorMeta> enumerate_actors(const std::string& filter = "");

  private:
    void build_command_tree();
    void execute_tokens(const std::vector<Token>& tokens);
    void print_greeting();
    static std::string get_history_path(const CliConfig& config);

    // Poll mailbox for a message with the given TypeTag, ignoring all others.
    // Returns the raw StreamBuffer payload if found before timeout.
    std::optional<StreamBuffer>
    poll_for_response(TypeTag expected_tag, std::chrono::milliseconds timeout);

    ActorSystem& system_;
    CliConfig config_;
    LineEditor line_editor_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    bool running_ = true;
};

} // namespace cli
} // namespace hpactor
