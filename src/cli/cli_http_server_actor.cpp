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

#include <hpactor/cli/cli_http_server_actor.hpp>

#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli.pb.h>

#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_gateway.hpp>
#include <hpactor/net/http_types.hpp>

#include "handlers/cli_http_handler_helpers.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace hpactor {
namespace cli {

// ── Forward declarations for handler functions ─────────────────────────
// (defined in src/cli/handlers/*.cpp)
namespace handlers {
void handle_legacy_post_cli(CliHttpServerActor* actor,
                            net::HTTPConnection* conn, net::HttpRequest&& req);
} // namespace handlers

// ── Route table PIMPL ──────────────────────────────────────────────────

struct CliHttpServerActor::RouteTable {
    std::vector<RouteEntry> routes;
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliHttpServerActor::CliHttpServerActor(ActorContext* ctx, ActorSystem& system,
                                       const CliHttpServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      gateway_(std::make_unique<net::HTTPGateway>()) {}

CliHttpServerActor::~CliHttpServerActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor interface
// ---------------------------------------------------------------------------

void CliHttpServerActor::on_daemon_start() {
    build_command_tree();
    init_routes();

    if (!gateway_->listen(config_.http_port, config_.http_bind_address)) {
        std::fprintf(stderr, "CliHttpServerActor: failed to listen on %s:%u\n",
                     config_.http_bind_address.c_str(),
                     static_cast<unsigned>(config_.http_port));
        listen_ok_ = false;
        return;
    }
    listen_ok_ = true;

    gateway_->set_request_handler(
        [this](net::HTTPConnection* conn, net::HttpRequest&& req) {
            dispatch_route(conn, std::move(req));
        });

    gateway_->set_max_connections(config_.max_connections);
}

bool CliHttpServerActor::run_once() {
    if (!listen_ok_ || !running_)
        return false;

    gateway_->run_once();
    return running_;
}

void CliHttpServerActor::on_daemon_stop() {
    running_ = false;
    if (gateway_) {
        gateway_->stop();
    }
    command_tree_.reset();
}

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

void CliHttpServerActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*root);
    command_tree_ = std::move(root);
}

// ---------------------------------------------------------------------------
// HTTP route dispatch
// ---------------------------------------------------------------------------

void CliHttpServerActor::init_routes() {
    route_table_ = std::make_unique<RouteTable>();
    route_table_->routes = {
        // Routes populated in subsequent tasks
    };

    // Legacy backward compat (Phase 1 only)
    if (config_.legacy_cli_endpoint) {
        route_table_->routes.push_back(
            {net::HttpMethod::POST, "/cli", handlers::handle_legacy_post_cli});
    }
}

void CliHttpServerActor::dispatch_route(net::HTTPConnection* conn,
                                        net::HttpRequest&& req) {
    for (const auto& route : route_table_->routes) {
        if (route.method != req.method)
            continue;
        req.path_params.clear();
        if (match_route_pattern(route.pattern, req.path, req.path_params)) {
            route.handler(this, conn, std::move(req));
            return;
        }
    }

    // No route matched
    send_error(conn, net::HttpStatusCode::NotFound, "NOT_FOUND",
               std::string(net::to_string(req.method)) + " " + req.path +
                   " has no handler");
}

// ---------------------------------------------------------------------------
// ISystemCliHost interface
// ---------------------------------------------------------------------------

void CliHttpServerActor::render_system_stats(OutputFormatter& output) {
    output.header("System Statistics");
    std::map<std::string, std::string> kv;
    kv["Total actors"] = std::to_string(system_.actor_count());
    if (auto* sched = system_.scheduler()) {
        kv["Scheduler threads"] = std::to_string(sched->worker_count());
    }
    output.key_value(kv);
}

void CliHttpServerActor::render_memory_stats(OutputFormatter& output) {
    output.header("Memory Regions");
    auto& reg = mem::MemoryRegionRegistry::instance();
    std::vector<std::string> cols = {"Region",     "Active", "Limit",
                                     "Pressure",   "Allocs", "Frees",
                                     "Corruptions"};
    std::vector<std::vector<std::string>> rows;
    static constexpr mem::RegionType kRegions[] = {
        mem::RegionType::kActor,     mem::RegionType::kMessage,
        mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
        mem::RegionType::kInternal,  mem::RegionType::kHibernate};
    for (auto region : kRegions) {
        auto snap = reg.snapshot(region);
        rows.push_back({
            mem::to_string(region),
            std::to_string(snap.active_bytes),
            std::to_string(snap.limit.hard_limit_bytes),
            mem::to_string(snap.pressure),
            std::to_string(snap.alloc_count),
            std::to_string(snap.free_count),
            std::to_string(snap.corruption_events),
        });
    }
    output.table(cols, rows);
}

void CliHttpServerActor::render_fault_status(OutputFormatter& output) {
    output.header("Fault Injection Status");
    auto& fc = system_.fault_controller();
    if (!fc.is_enabled()) {
        output.raw("Fault injection is disabled.\n");
        return;
    }
    std::map<std::string, std::string> kv;
    kv["Enabled"] = "yes";
    kv["Seed"] = std::to_string(fc.replay_seed());
    kv["Hooks triggered"] = std::to_string(fc.faults_fired());
    output.key_value(kv);
}

void CliHttpServerActor::render_dlq_list(OutputFormatter& output,
                                         std::string_view filter) {
    output.header("Dead Letter Queue");
    auto* dlq = system_.dead_letter_queue();
    if (!dlq) {
        output.raw("DLQ is not configured.\n");
        return;
    }
    auto records = dlq->snapshot_records();
    if (records.empty()) {
        output.raw("DLQ is empty.\n");
        return;
    }
    std::vector<std::string> cols = {"#", "Actor", "Reason", "Source", "Age"};
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < records.size(); ++i) {
        auto& r = records[i];
        if (!filter.empty()) {
            std::string aid = std::to_string(r.target.id.value());
            if (aid.find(filter) == std::string::npos)
                continue;
        }
        rows.push_back({
            std::to_string(i),
            std::to_string(r.target.id.value()),
            mailbox::to_string(r.reason),
            mailbox::to_string(r.source),
            std::to_string(r.timestamp_ns / 1'000'000) + "ms",
        });
    }
    output.table(cols, rows);
}

result<void> CliHttpServerActor::dlq_replay(uint32_t index, ActorId target) {
    auto* dlq = system_.dead_letter_queue();
    if (!dlq)
        return result<void>::make(
            error(errors::actor_not_found, "DLQ not configured"));

    mailbox::DeadLetterRecord record;
    if (!dlq->try_pop_at(index, record))
        return result<void>::make(
            error(errors::invalid_argument, "DLQ index out of range"));

    TypedMessage msg(record.type_tag, std::move(record.payload_sample));
    msg.set_sender_address(address());
    auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
    if (!enqueue_result.accepted())
        return result<void>::make(
            error(errors::mailbox_full, "replay delivery failed"));

    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface
// ---------------------------------------------------------------------------

result<void> CliHttpServerActor::drain() {
    return system_.shutdown();
}

result<void> CliHttpServerActor::shutdown() {
    return system_.shutdown();
}

// ---------------------------------------------------------------------------
// ICliCommandHost interface
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliHttpServerActor::inspect(ActorId target, const InspectStateRequest& req,
                            std::chrono::milliseconds timeout) {
    TypedMessage msg(TypeTag::InspectStateRequestTag, req);
    msg.set_sender_address(address());

    auto enq = system_.try_deliver_local(target, std::move(msg));
    if (!enq.accepted())
        return std::nullopt;

    // Poll mailbox for reply with timeout
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage m;
        if (mailbox()->try_pop(m)) {
            if (m.type_id() == TypeTag::InspectStateResponseTag) {
                auto reply = m.as<InspectStateReply>();
                if (reply)
                    return *reply;
                return std::nullopt;
            }
            // Drop unrelated messages (e.g., scheduled timers)
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

std::optional<KillReply>
CliHttpServerActor::kill(ActorId target, const KillRequest& req,
                         std::chrono::milliseconds timeout) {
    TypedMessage msg(TypeTag::KillRequestTag, req);
    msg.set_sender_address(address());

    auto enq = system_.try_deliver_local(target, std::move(msg));
    if (!enq.accepted())
        return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage m;
        if (mailbox()->try_pop(m)) {
            if (m.type_id() == TypeTag::KillResponseTag) {
                auto reply = m.as<KillReply>();
                if (reply)
                    return *reply;
                return std::nullopt;
            }
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

std::optional<QuarantineReply>
CliHttpServerActor::quarantine(ActorId target, const QuarantineRequest& req,
                               std::chrono::milliseconds timeout) {
    TypedMessage msg(TypeTag::QuarantineRequestTag, req);
    msg.set_sender_address(address());

    auto enq = system_.try_deliver_local(target, std::move(msg));
    if (!enq.accepted())
        return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage m;
        if (mailbox()->try_pop(m)) {
            if (m.type_id() == TypeTag::QuarantineResponseTag) {
                auto reply = m.as<QuarantineReply>();
                if (reply)
                    return *reply;
                return std::nullopt;
            }
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

std::vector<ActorMeta> CliHttpServerActor::enumerate(std::string_view filter) {
    std::vector<ActorMeta> result;
    system_.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        auto meta = actor.to_metadata();
        if (!filter.empty()) {
            // Filter by type name substring match
            if (meta.actor_type.find(filter) == std::string::npos)
                return;
        }
        result.push_back(std::move(meta));
    });
    return result;
}

} // namespace cli
} // namespace hpactor
