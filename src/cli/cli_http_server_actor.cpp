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
#include <hpactor/msg/message_id.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_gateway.hpp>
#include <hpactor/net/http_types.hpp>

#include <hpactor/metrics/metrics_actor.hpp>

#include "handlers/cli_http_handler_helpers.hpp"

#include "commands/command_utils.hpp"

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

// Actor handlers
void handle_list_actors(CliHttpServerActor* actor, net::HTTPConnection* conn,
                        net::HttpRequest&& req);
void handle_get_actor(CliHttpServerActor* actor, net::HTTPConnection* conn,
                      net::HttpRequest&& req);
void handle_kill_actor(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req);
void handle_get_mailbox(CliHttpServerActor* actor, net::HTTPConnection* conn,
                        net::HttpRequest&& req);
void handle_get_children(CliHttpServerActor* actor, net::HTTPConnection* conn,
                         net::HttpRequest&& req);
void handle_get_circuit_breaker(CliHttpServerActor* actor,
                                net::HTTPConnection* conn, net::HttpRequest&& req);
void handle_reset_circuit_breaker(CliHttpServerActor* actor,
                                  net::HTTPConnection* conn,
                                  net::HttpRequest&& req);
void handle_quarantine_actor(CliHttpServerActor* actor,
                             net::HTTPConnection* conn, net::HttpRequest&& req);
void handle_unquarantine_actor(CliHttpServerActor* actor,
                               net::HTTPConnection* conn, net::HttpRequest&& req);
void handle_get_actor_memory(CliHttpServerActor* actor,
                             net::HTTPConnection* conn, net::HttpRequest&& req);

// System handlers
void handle_api_index(CliHttpServerActor* actor, net::HTTPConnection* conn,
                      net::HttpRequest&& req);
void handle_get_system(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req);
void handle_get_system_stats(CliHttpServerActor* actor,
                             net::HTTPConnection* conn, net::HttpRequest&& req);
void handle_get_system_memory(CliHttpServerActor* actor,
                              net::HTTPConnection* conn, net::HttpRequest&& req);
void handle_drain(CliHttpServerActor* actor, net::HTTPConnection* conn,
                  net::HttpRequest&& req);
void handle_shutdown(CliHttpServerActor* actor, net::HTTPConnection* conn,
                     net::HttpRequest&& req);

// Fault handlers
void handle_get_faults(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req);
void handle_clear_faults(CliHttpServerActor* actor, net::HTTPConnection* conn,
                         net::HttpRequest&& req);

// DLQ handlers
void handle_list_dlq(CliHttpServerActor* actor, net::HTTPConnection* conn,
                     net::HttpRequest&& req);
void handle_get_dlq_record(CliHttpServerActor* actor, net::HTTPConnection* conn,
                           net::HttpRequest&& req);
void handle_replay_dlq(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req);
void handle_export_dlq(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req);

// Ask handlers
void handle_list_asks(CliHttpServerActor* actor, net::HTTPConnection* conn,
                      net::HttpRequest&& req);
void handle_get_ask(CliHttpServerActor* actor, net::HTTPConnection* conn,
                    net::HttpRequest&& req);
void handle_cancel_ask(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req);
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
      gateway_(std::make_unique<net::HTTPGateway>()), host_impl_(system_) {}

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
    command_tree_ = host_impl_.build_command_tree();
}

// ---------------------------------------------------------------------------
// HTTP route dispatch
// ---------------------------------------------------------------------------

void CliHttpServerActor::init_routes() {
    route_table_ = std::make_unique<RouteTable>();
    static constexpr std::string_view kApiV1 = "/api/v1";
    route_table_->routes = {
        // Actor handlers
        {net::HttpMethod::GET, std::string(kApiV1) + "/actors",
         handlers::handle_list_actors},
        {net::HttpMethod::GET, std::string(kApiV1) + "/actors/:id",
         handlers::handle_get_actor},
        {net::HttpMethod::DELETE, std::string(kApiV1) + "/actors/:id",
         handlers::handle_kill_actor},
        {net::HttpMethod::GET, std::string(kApiV1) + "/actors/:id/mailbox",
         handlers::handle_get_mailbox},
        {net::HttpMethod::GET, std::string(kApiV1) + "/actors/:id/children",
         handlers::handle_get_children},
        {net::HttpMethod::GET, std::string(kApiV1) + "/actors/:id/circuit-breaker",
         handlers::handle_get_circuit_breaker},
        {net::HttpMethod::POST,
         std::string(kApiV1) + "/actors/:id/circuit-breaker/reset",
         handlers::handle_reset_circuit_breaker},
        {net::HttpMethod::POST, std::string(kApiV1) + "/actors/:id/quarantine",
         handlers::handle_quarantine_actor},
        {net::HttpMethod::DELETE, std::string(kApiV1) + "/actors/:id/quarantine",
         handlers::handle_unquarantine_actor},
        {net::HttpMethod::GET, std::string(kApiV1) + "/actors/:id/memory",
         handlers::handle_get_actor_memory},

        // System handlers
        {net::HttpMethod::GET, std::string(kApiV1), handlers::handle_api_index},
        {net::HttpMethod::GET, std::string(kApiV1) + "/system",
         handlers::handle_get_system},
        {net::HttpMethod::GET, std::string(kApiV1) + "/system/stats",
         handlers::handle_get_system_stats},
        {net::HttpMethod::GET, std::string(kApiV1) + "/system/memory",
         handlers::handle_get_system_memory},
        {net::HttpMethod::POST, std::string(kApiV1) + "/system/drain",
         handlers::handle_drain},
        {net::HttpMethod::POST, std::string(kApiV1) + "/system/shutdown",
         handlers::handle_shutdown},

        // Fault handlers
        {net::HttpMethod::GET, std::string(kApiV1) + "/faults",
         handlers::handle_get_faults},
        {net::HttpMethod::POST, std::string(kApiV1) + "/faults/clear",
         handlers::handle_clear_faults},

        // DLQ handlers
        {net::HttpMethod::GET, std::string(kApiV1) + "/dlq",
         handlers::handle_list_dlq},
        {net::HttpMethod::GET, std::string(kApiV1) + "/dlq/export",
         handlers::handle_export_dlq},
        {net::HttpMethod::GET, std::string(kApiV1) + "/dlq/:index",
         handlers::handle_get_dlq_record},
        {net::HttpMethod::POST, std::string(kApiV1) + "/dlq/:index/replay",
         handlers::handle_replay_dlq},

        // Ask handlers
        {net::HttpMethod::GET, std::string(kApiV1) + "/asks",
         handlers::handle_list_asks},
        {net::HttpMethod::GET, std::string(kApiV1) + "/asks/:message_id",
         handlers::handle_get_ask},
        {net::HttpMethod::DELETE, std::string(kApiV1) + "/asks/:message_id",
         handlers::handle_cancel_ask},
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

// execute_path is inline in header (returns false — local host).

result<void> CliHttpServerActor::dlq_replay(uint32_t index, ActorId target) {
    return host_impl_.dlq_replay(index, target, address());
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface
// ---------------------------------------------------------------------------

result<void> CliHttpServerActor::drain() {
    return host_impl_.drain();
}

result<void> CliHttpServerActor::shutdown() {
    return host_impl_.shutdown();
}

// ---------------------------------------------------------------------------
// ICliCommandHost interface
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliHttpServerActor::inspect(ActorId target, const InspectStateRequest& req,
                            std::chrono::milliseconds timeout) {
    return host_impl_.inspect(target, req, mailbox(), address(), timeout);
}

std::optional<KillReply>
CliHttpServerActor::kill(ActorId target, const KillRequest& req,
                         std::chrono::milliseconds timeout) {
    return host_impl_.kill(target, req, mailbox(), address(), timeout);
}

std::optional<QuarantineReply>
CliHttpServerActor::quarantine(ActorId target, const QuarantineRequest& req,
                               std::chrono::milliseconds timeout) {
    return host_impl_.quarantine(target, req, mailbox(), address(), timeout);
}

std::vector<ActorMeta> CliHttpServerActor::enumerate(std::string_view filter) {
    return host_impl_.enumerate(filter);
}

} // namespace cli
} // namespace hpactor
