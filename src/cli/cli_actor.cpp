// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/line_editor.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/types/failure_reason.hpp>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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

static ActorId parse_actor_id(const std::string& s) {
    uint64_t raw = 0;
    int base = 10;
    const char* start = s.data();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = s.data() + 2;
    }
    auto [ptr, ec] = std::from_chars(start, s.data() + s.size(), raw, base);
    if (ec != std::errc{})
        return ActorId{0};
    return ActorId{raw};
}

void CliActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");

    // ── /actor <id> show ──────────────────────────────────────────────
    auto* actor_cmd = root->add_child("actor", "Actor operations");
    auto* actor_id_param =
        actor_cmd->add_child("<id>", "Target actor ID", /*is_param=*/true);

    actor_id_param
        ->add_child("show", "Display actor metadata, state, mailbox, and "
                            "children")
        ->execute = [this](CommandContext& ctx) -> result<void> {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> show)");
            return result<void>::make();
        }
        ActorId target_id = parse_actor_id(*id_str);
        if (target_id == ActorId{0}) {
            ctx.output->error("Invalid actor ID: " + *id_str);
            return result<void>::make();
        }

        InspectStateRequest req;
        req.set_target_actor_id(target_id.value());
        req.set_include_state(true);
        req.set_include_mailbox(true);
        req.set_include_children(true);

        auto reply = send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        ctx.output->header("Actor " + *id_str + " — " +
                           reply->metadata().actor_type());

        std::map<std::string, std::string> kv;
        kv["State"] = reply->metadata().state();
        kv["Incarnation"] = std::to_string(reply->metadata().incarnation());
        kv["Processed"] =
            std::to_string(reply->metadata().messages_processed()) + " msgs";
        kv["Uptime (ms)"] = std::to_string(reply->metadata().uptime_ms());
        kv["Behavior"] = reply->metadata().behavior_name();

        if (reply->has_mailbox()) {
            kv["Mailbox depth"] = std::to_string(reply->mailbox().depth());
            kv["Mailbox max"] = std::to_string(reply->mailbox().max_depth());
        }

        ctx.output->key_value(kv);

        if (!reply->state_blob().empty()) {
            ctx.output->raw("State: " + reply->state_blob());
        }

        return result<void>::make();
    };

    // ── /actor <id> kill ──────────────────────────────────────────────
    actor_id_param->add_child("kill", "Terminate actor (graceful shutdown)")->execute =
        [this](CommandContext& ctx) -> result<void> {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> kill)");
            return result<void>::make();
        }
        ActorId target_id = parse_actor_id(*id_str);
        if (target_id == ActorId{0}) {
            ctx.output->error("Invalid actor ID: " + *id_str);
            return result<void>::make();
        }

        KillRequest req;
        req.set_target_actor_id(target_id.value());
        req.set_force(false);

        auto reply = send_and_wait_kill(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        if (reply->success()) {
            ctx.output->raw("Actor " + *id_str + " terminated.");
        } else {
            ctx.output->error("Failed to kill actor " + *id_str + ": " +
                              reply->error_message());
        }
        return result<void>::make();
    };

    // ── /actor list ───────────────────────────────────────────────────
    actor_cmd->add_child("list", "List all actors [--filter <type>]")->execute =
        [this](CommandContext& ctx) -> result<void> {
        std::string filter;
        if (auto f = ctx.get_param("filter"))
            filter = *f;

        auto actors = enumerate_actors(filter);

        ctx.output->header("Actors (" + std::to_string(actors.size()) + " total)");

        std::vector<std::string> cols = {"ID", "Type", "State", "Processed"};
        std::vector<std::vector<std::string>> rows;
        rows.reserve(actors.size());

        for (auto& a : actors) {
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "0x%04llX",
                     static_cast<unsigned long long>(a.actor_id));
            rows.push_back({id_buf, a.actor_type, a.state,
                            std::to_string(a.messages_processed)});
        }

        ctx.output->table(cols, rows);
        return result<void>::make();
    };

    // ── /system stats ─────────────────────────────────────────────────
    auto* sys = root->add_child("system", "System operations");

    sys->add_child("stats", "System-wide statistics")->execute =
        [this](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Statistics");

        std::map<std::string, std::string> kv;
        kv["Total actors"] = std::to_string(system_.actor_count());
        if (auto* sched = system_.scheduler()) {
            kv["Scheduler threads"] = std::to_string(sched->worker_count());
        }
        kv["CLI enabled"] = config_.enabled ? "yes" : "no";
        kv["CLI format"] = config_.default_format;

        ctx.output->key_value(kv);
        return result<void>::make();
    };

    sys->add_child("memory", "Memory subsystem stats")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Memory");
        ctx.output->key_value({{"Status", "Memory subsystem active"},
                               {"Note", "Use /metrics show for detailed memory "
                                        "stats"}});
        return result<void>::make();
    };

    sys->add_child("list", "List system actors")->execute =
        [this](CommandContext& ctx) -> result<void> {
        ctx.output->header("System Actors");

        std::vector<std::string> cols = {"ID", "Type", "State"};
        std::vector<std::vector<std::string>> rows;

        system_.for_each_actor([&](ActorId actor_id, AbstractActor& actor) {
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "0x%04llX",
                     static_cast<unsigned long long>(actor_id.value()));
            auto meta = actor.to_metadata();
            rows.push_back({id_buf, meta.actor_type, meta.state});
        });

        ctx.output->table(cols, rows);
        return result<void>::make();
    };

    // ── /system drain ──────────────────────────────────────────────────
    auto* drain = sys->add_child("drain", "Graceful node shutdown");
    drain->execute = [this](CommandContext& ctx) -> result<void> {
        auto shutdown_result = system().shutdown();
        if (shutdown_result.has_value()) {
            ctx.output->raw("Shutdown complete");
        } else {
            ctx.output->error("Shutdown failed");
        }
        return result<void>::make();
    };

    // ── /system drain status ───────────────────────────────────────────
    drain->add_child("status", "Show shutdown progress")->execute =
        [this](CommandContext& ctx) -> result<void> {
        ctx.output->raw("Shutdown phase: " + std::to_string(static_cast<int>(
                                                 system().shutdown_phase())));
        ctx.output->raw("Actors live: " + std::to_string(system().actor_count()));
        return result<void>::make();
    };

    // ── /system stop <actor_id> ────────────────────────────────────────
    auto* stop = sys->add_child("stop", "Graceful stop of an actor");
    auto* stop_id_param = stop->add_child("<actor_id>", "Actor ID to stop",
                                          /*is_param=*/true);
    stop_id_param->execute = [this](CommandContext& ctx) -> result<void> {
        auto id_str = ctx.get_param("<actor_id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /system stop "
                              "<actor_id>)");
            return result<void>::make();
        }
        ActorId target_id = parse_actor_id(*id_str);
        if (target_id == ActorId{0}) {
            ctx.output->error("Invalid actor ID: " + *id_str);
            return result<void>::make();
        }

        if (ctx.has_flag("force")) {
            system().set_drain_config(target_id,
                                      DrainConfig{DrainPolicy::ImmediateStop});
        }

        auto actor = system().get_actor(target_id);
        if (!actor) {
            ctx.output->error("Actor not found: " + std::string(*id_str));
            return result<void>::make();
        }
        context()->stop(target_id);
        ctx.output->raw("Drain initiated for actor " + std::string(*id_str));
        return result<void>::make();
    };

    // ── /failure ────────────────────────────────────────────────────────
    auto* failure_cmd = root->add_child("failure", "Failure envelope "
                                                   "operations");

    failure_cmd->add_child("reasons", "List all canonical failure reasons")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Canonical Failure Reasons");

        std::vector<std::string> cols = {"Reason", "Code", "Retryable"};
        std::vector<std::vector<std::string>> rows;

        auto add_row = [&](FailureReason r, uint8_t code) {
            rows.push_back({std::string(to_string(r)), std::to_string(code),
                            retryable(r) ? "yes" : "no"});
        };

        add_row(FailureReason::NoRoute, 0);
        add_row(FailureReason::NodeUnavailable, 1);
        add_row(FailureReason::ActorDead, 10);
        add_row(FailureReason::ActorNotReady, 11);
        add_row(FailureReason::Quarantined, 12);
        add_row(FailureReason::CircuitOpen, 13);
        add_row(FailureReason::MailboxFull, 20);
        add_row(FailureReason::OutboundQueueFull, 21);
        add_row(FailureReason::MemoryPressure, 22);
        add_row(FailureReason::Expired, 30);
        add_row(FailureReason::Timeout, 31);
        add_row(FailureReason::RejectedByPolicy, 40);
        add_row(FailureReason::Dropped, 41);
        add_row(FailureReason::MailboxClosed, 42);
        add_row(FailureReason::SerializationError, 50);
        add_row(FailureReason::TransportError, 51);
        add_row(FailureReason::FrameRejected, 52);
        add_row(FailureReason::Duplicate, 60);
        add_row(FailureReason::Draining, 70);
        add_row(FailureReason::ShuttingDown, 71);
        add_row(FailureReason::RetryExhausted, 80);
        add_row(FailureReason::SpawnFailed, 90);
        add_row(FailureReason::Unknown, 255);

        ctx.output->table(cols, rows);
        return result<void>::make();
    };

    failure_cmd->add_child("summary", "Show failure subsystem status")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Failure Subsystem Status");

        std::map<std::string, std::string> kv;
        kv["FailureReason values"] = "23";
        kv["FailureSource values"] = "12";
        kv["DLQ mapping"] = "13 DeadLetterReason codes mapped";
        kv["Spawn mapping"] = "6 spawn_errors codes mapped";
        kv["EnqueueResultCode mapping"] = "9 mailbox codes mapped";
        kv["Delivery failure metric"] = "kDeliveryFailure wired in "
                                        "try_deliver_local";
        kv["Phase"] = "1-3 complete, 4 (CLI) in progress";

        ctx.output->key_value(kv);
        ctx.output->raw("Use /failure reasons for the full reason table.");
        ctx.output->raw("Use /actor <id> show for per-actor mailbox/depth "
                        "stats.");
        return result<void>::make();
    };

    // ── /metrics show ─────────────────────────────────────────────────
    auto* metrics = root->add_child("metrics", "Metrics operations");
    metrics->add_child("show", "Show current metrics snapshot")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Metrics");
        ctx.output->raw("metrics show — not yet implemented");
        return result<void>::make();
    };

    // ── /topology show ────────────────────────────────────────────────
    auto* topo = root->add_child("topology", "Topology operations");
    topo->add_child("show", "Show topology tree")->execute =
        [](CommandContext& ctx) -> result<void> {
        ctx.output->header("Topology");
        ctx.output->raw("topology show — not yet implemented");
        return result<void>::make();
    };

    // ── /help ─────────────────────────────────────────────────────────
    root->add_child("help", "Show available commands")->execute =
        [this](CommandContext& ctx) -> result<void> {
        ctx.output->header("Available Commands");
        ctx.output->raw(command_tree_->help());
        return result<void>::make();
    };

    // ── /quit ─────────────────────────────────────────────────────────
    root->add_child("quit", "Exit the CLI")->execute =
        [this](CommandContext& ctx) -> result<void> {
        ctx.output->raw("Goodbye.");
        running_ = false;
        return result<void>::make();
    };

    command_tree_ = std::move(root);
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

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
