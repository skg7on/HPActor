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
            ctx.output->header("Details");
            ctx.output->raw(reply->state_blob());
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

class ActorRateCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/rate";
    }
    std::string_view help_text() const noexcept override {
        return "Show actor rate limiter state";
    }
    int order() const noexcept override {
        return 270;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> rate)");
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
        req.set_include_mailbox(true);
        req.set_include_rate_limiter(true);

        auto reply = cli->send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        ctx.output->header("Rate Limiter — Actor " + *id_str);

        std::map<std::string, std::string> kv;
        auto& mbox = reply->mailbox();
        kv["Enabled"] = mbox.rate_limiter_enabled() ? "true" : "false";
        if (mbox.rate_limiter_enabled()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f", mbox.rate_limiter_rate());
            kv["Rate (msg/s)"] = buf;
            kv["Burst"] = std::to_string(mbox.rate_limiter_burst());
            snprintf(buf, sizeof(buf), "%.1f", mbox.rate_limiter_current_tokens());
            kv["Current tokens"] = buf;
        }
        kv["Blocked total"] = std::to_string(mbox.rate_limit_blocked_total());
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

class ActorAdmissionCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/admission";
    }
    std::string_view help_text() const noexcept override {
        return "Show actor admission policy state";
    }
    int order() const noexcept override {
        return 280;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> "
                              "admission)");
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
        req.set_include_mailbox(true);
        req.set_include_admission(true);

        auto reply = cli->send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        ctx.output->header("Admission Policy — Actor " + *id_str);

        std::map<std::string, std::string> kv;
        auto& mbox = reply->mailbox();
        kv["Active policies"] = std::to_string(mbox.admission_policy_count());
        kv["Rejected total"] = std::to_string(mbox.admission_rejected_total());
        kv["DLQ routed total"] = std::to_string(mbox.admission_dlq_routed_total());
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

class ActorDeliveryCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/delivery";
    }
    std::string_view help_text() const noexcept override {
        return "Show per-actor delivery result counters";
    }
    int order() const noexcept override {
        return 285;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> delivery)");
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
        req.set_include_mailbox(true);

        auto reply = cli->send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        ctx.output->header("Delivery Results — Actor " + *id_str);

        std::map<std::string, std::string> kv;
        auto& mbox = reply->mailbox();
        kv["Accepted"] = std::to_string(mbox.delivery_accepted_total());
        kv["Rejected"] = std::to_string(mbox.delivery_rejected_total());
        kv["Failed (not retryable)"] =
            std::to_string(mbox.delivery_failed_total());
        kv["Retryable"] = std::to_string(mbox.delivery_retryable_total());
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

class ActorDeliveryStatsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/delivery-stats";
    }
    std::string_view help_text() const noexcept override {
        return "Show delivery statistics with accept/reject/retry ratios";
    }
    int order() const noexcept override {
        return 286;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> "
                              "delivery-stats)");
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
        req.set_include_mailbox(true);

        auto reply = cli->send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str +
                              " (timeout or not found)");
            return result<void>::make();
        }

        ctx.output->header("Delivery Statistics — Actor " + *id_str);

        auto& mbox = reply->mailbox();
        uint64_t accepted = mbox.delivery_accepted_total();
        uint64_t rejected = mbox.delivery_rejected_total();
        uint64_t failed = mbox.delivery_failed_total();
        uint64_t retryable = mbox.delivery_retryable_total();
        uint64_t total = accepted + rejected;

        std::map<std::string, std::string> kv;
        kv["Accepted"] = std::to_string(accepted);
        kv["Rejected"] = std::to_string(rejected);
        kv["Failed (not retryable)"] = std::to_string(failed);
        kv["Retryable"] = std::to_string(retryable);

        if (total > 0) {
            char buf[32];
            double accept_rate = 100.0 * static_cast<double>(accepted) /
                                 static_cast<double>(total);
            snprintf(buf, sizeof(buf), "%.1f%%", accept_rate);
            kv["Accept rate"] = buf;

            double retry_rate = 100.0 * static_cast<double>(retryable) /
                                static_cast<double>(total);
            snprintf(buf, sizeof(buf), "%.1f%%", retry_rate);
            kv["Retryable rate"] = buf;

            double fail_rate =
                100.0 * static_cast<double>(failed) / static_cast<double>(total);
            snprintf(buf, sizeof(buf), "%.1f%%", fail_rate);
            kv["Fail rate"] = buf;
        } else {
            kv["Accept rate"] = "N/A (no deliveries)";
            kv["Retryable rate"] = "N/A (no deliveries)";
            kv["Fail rate"] = "N/A (no deliveries)";
        }

        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

class ActorLinksCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/links";
    }
    std::string_view help_text() const noexcept override {
        return "Show linked and monitored actors";
    }
    int order() const noexcept override {
        return 287;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> links)");
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

        auto reply = cli->send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str);
            return result<void>::make();
        }

        ctx.output->header("Links — Actor " + *id_str);
        if (!reply->state_blob().empty()) {
            ctx.output->raw(reply->state_blob());
        } else {
            ctx.output->raw("No link information available for this actor.");
        }
        return result<void>::make();
    }
};

class ActorBackpressureCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "actor/<id>/backpressure";
    }
    std::string_view help_text() const noexcept override {
        return "Show backpressure signal state and mailbox pressure";
    }
    int order() const noexcept override {
        return 288;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto id_str = ctx.get_param("<id>");
        if (!id_str) {
            ctx.output->error("Missing actor ID (usage: /actor <id> backpressure)");
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
        req.set_include_mailbox(true);

        auto reply = cli->send_and_wait_inspect(target_id, req);
        if (!reply) {
            ctx.output->error("No response from actor " + *id_str);
            return result<void>::make();
        }

        auto& mbox = reply->mailbox();
        ctx.output->header("Backpressure — Actor " + *id_str);

        std::map<std::string, std::string> kv;
        kv["Pressure state"] = mbox.pressure_state();
        char depth_buf[64];
        snprintf(depth_buf, sizeof(depth_buf), "%u/%u (%.1f%%)", mbox.depth(),
                 mbox.capacity(),
                 mbox.capacity() > 0
                     ? 100.0 * mbox.depth() / static_cast<double>(mbox.capacity())
                     : 0.0);
        kv["Depth"] = depth_buf;
        char byte_buf[64];
        snprintf(byte_buf, sizeof(byte_buf), "%llu / %llu",
                 static_cast<unsigned long long>(mbox.queued_bytes()),
                 static_cast<unsigned long long>(mbox.byte_capacity()));
        kv["Byte utilization"] = byte_buf;
        kv["Total rejected"] = std::to_string(mbox.total_rejected());
        kv["Total dropped"] = std::to_string(mbox.total_dropped());
        kv["Total dead letters"] = std::to_string(mbox.total_dead_letters());
        kv["Overflow policy"] = mbox.overflow_policy();
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

const CommandRegistration<ActorShowCommand> kRegisterActorShow;
const CommandRegistration<ActorCircuitCommand> kRegisterActorCircuit;
const CommandRegistration<ActorKillCommand> kRegisterActorKill;
const CommandRegistration<ActorQuarantineCommand> kRegisterActorQuarantine;
const CommandRegistration<ActorUnquarantineCommand> kRegisterActorUnquarantine;
const CommandRegistration<ActorListCommand> kRegisterActorList;
const CommandRegistration<ActorRateCommand> kRegisterActorRate;
const CommandRegistration<ActorAdmissionCommand> kRegisterActorAdmission;
const CommandRegistration<ActorDeliveryCommand> kRegisterActorDelivery;
const CommandRegistration<ActorDeliveryStatsCommand> kRegisterActorDeliveryStats;
const CommandRegistration<ActorLinksCommand> kRegisterActorLinks;
const CommandRegistration<ActorBackpressureCommand> kRegisterActorBackpressure;

} // anonymous namespace
} // namespace cli
} // namespace hpactor