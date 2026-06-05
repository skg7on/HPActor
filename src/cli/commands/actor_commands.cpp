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
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>

#include "command_utils.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class ActorShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/show";
    }
    std::string_view help_text() const noexcept override {
        return "Display actor metadata, state, mailbox, and children";
    }
    int order() const noexcept override {
        return 100;
    }

    result<void> execute(CommandContext& ctx) const override {
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

        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error: no CLI actor");
            return result<void>::make();
        }

        InspectStateRequest req;
        req.set_target_actor_id(target_id.value());
        req.set_include_state(true);
        req.set_include_mailbox(true);
        req.set_include_children(true);

        auto reply = cli->send_and_wait_inspect(target_id, req);
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
            const auto& mailbox = reply->mailbox();
            kv["Mailbox depth"] = std::to_string(mailbox.depth()) + "/" +
                                  std::to_string(mailbox.capacity());
            kv["Mailbox bytes"] = std::to_string(mailbox.queued_bytes()) + "/" +
                                  std::to_string(mailbox.byte_capacity());
            kv["Mailbox pressure"] = mailbox.pressure_state();
            kv["Mailbox overflow"] = mailbox.overflow_policy();
            kv["Mailbox rejected"] = std::to_string(mailbox.total_rejected());
            kv["Mailbox dropped"] = std::to_string(mailbox.total_dropped());
            kv["Mailbox dead letters"] =
                std::to_string(mailbox.total_dead_letters());
            kv["Mailbox max"] = std::to_string(mailbox.max_depth());
        }

        ctx.output->key_value(kv);

        if (!reply->state_blob().empty()) {
            ctx.output->raw("State: " + reply->state_blob());
        }
        return result<void>::make();
    }
};

class ActorKillCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/kill";
    }
    std::string_view help_text() const noexcept override {
        return "Terminate actor (graceful shutdown)";
    }
    int order() const noexcept override {
        return 200;
    }

    result<void> execute(CommandContext& ctx) const override {
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

        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error: no CLI actor");
            return result<void>::make();
        }

        KillRequest req;
        req.set_target_actor_id(target_id.value());
        req.set_force(false);

        auto reply = cli->send_and_wait_kill(target_id, req);
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
    }
};

class ActorListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/list";
    }
    std::string_view help_text() const noexcept override {
        return "List all actors [--filter <type>]";
    }
    int order() const noexcept override {
        return 300;
    }

    result<void> execute(CommandContext& ctx) const override {
        std::string filter;
        if (auto f = ctx.get_param("filter"))
            filter = *f;

        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error: no CLI actor");
            return result<void>::make();
        }

        auto actors = cli->enumerate_actors(filter);

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
    }
};

class ActorCircuitCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/circuit";
    }
    std::string_view help_text() const noexcept override {
        return "Show circuit breaker state: state, trip count, failure EMA";
    }
    int order() const noexcept override {
        return 150;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> circuit)");
            return result<void>::make();
        }
        ActorId target_id = parse_actor_id(*id_str);
        if (target_id == ActorId{0}) {
            ctx.output->error("Invalid actor ID: " + *id_str);
            return result<void>::make();
        }

        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error: no CLI actor");
            return result<void>::make();
        }

        InspectStateRequest req;
        req.set_target_actor_id(target_id.value());
        req.set_include_circuit_breaker(true);
        req.set_include_quarantine_info(true);

        auto reply = cli->send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        if (!reply->quarantine_enabled()) {
            ctx.output->raw("Circuit breaker is not enabled for actor " +
                            *id_str + ".");
            return result<void>::make();
        }

        ctx.output->header("Circuit Breaker — Actor " + *id_str);

        std::map<std::string, std::string> kv;
        kv["State"] = reply->circuit_breaker().state();
        kv["Trip count"] = std::to_string(reply->circuit_breaker().trip_count());
        char ema_buf[32];
        snprintf(ema_buf, sizeof(ema_buf), "%.3f",
                 reply->circuit_breaker().failure_ema());
        kv["Failure EMA"] = ema_buf;
        if (reply->circuit_breaker().opened_at_ns() > 0) {
            kv["Opened at (ns)"] =
                std::to_string(reply->circuit_breaker().opened_at_ns());
        }
        if (!reply->quarantine_reason().empty()) {
            kv["Quarantine reason"] = reply->quarantine_reason();
        }

        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

class ActorQuarantineCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/quarantine";
    }
    std::string_view help_text() const noexcept override {
        return "Manually quarantine an actor [--reason <text>]";
    }
    int order() const noexcept override {
        return 250;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> quarantine "
                              "[--reason "
                              "<text>])");
            return result<void>::make();
        }
        ActorId target_id = parse_actor_id(*id_str);
        if (target_id == ActorId{0}) {
            ctx.output->error("Invalid actor ID: " + *id_str);
            return result<void>::make();
        }

        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error: no CLI actor");
            return result<void>::make();
        }

        QuarantineRequest req;
        req.set_target_actor_id(target_id.value());
        req.set_unquarantine(false);
        if (auto reason = ctx.get_param("reason")) {
            req.set_reason(*reason);
        }

        auto reply = cli->send_and_wait_quarantine(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        if (reply->success()) {
            ctx.output->raw("Actor " + *id_str + " quarantined.");
        } else {
            ctx.output->error("Failed to quarantine actor " + *id_str + ": " +
                              reply->error_message());
        }
        return result<void>::make();
    }
};

class ActorUnquarantineCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/unquarantine";
    }
    std::string_view help_text() const noexcept override {
        return "Release an actor from quarantine";
    }
    int order() const noexcept override {
        return 260;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> "
                              "unquarantine)");
            return result<void>::make();
        }
        ActorId target_id = parse_actor_id(*id_str);
        if (target_id == ActorId{0}) {
            ctx.output->error("Invalid actor ID: " + *id_str);
            return result<void>::make();
        }

        auto* cli = ctx.cli_actor;
        if (!cli) {
            ctx.output->error("Internal error: no CLI actor");
            return result<void>::make();
        }

        QuarantineRequest req;
        req.set_target_actor_id(target_id.value());
        req.set_unquarantine(true);

        auto reply = cli->send_and_wait_quarantine(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        if (reply->success()) {
            ctx.output->raw("Actor " + *id_str + " released from quarantine.");
        } else {
            ctx.output->error("Failed to unquarantine actor " + *id_str + ": " +
                              reply->error_message());
        }
        return result<void>::make();
    }
};

const CommandRegistration<ActorShowCommand> kRegisterActorShow;
const CommandRegistration<ActorCircuitCommand> kRegisterActorCircuit;
const CommandRegistration<ActorKillCommand> kRegisterActorKill;
const CommandRegistration<ActorQuarantineCommand> kRegisterActorQuarantine;
const CommandRegistration<ActorUnquarantineCommand> kRegisterActorUnquarantine;
const CommandRegistration<ActorListCommand> kRegisterActorList;

} // anonymous namespace
} // namespace cli
} // namespace hpactor