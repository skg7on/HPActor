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
#include <hpactor/cli/http_handler.hpp>
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

// ── Handler registration functions (defined in src/cli/handlers/*.cpp) ─
namespace handlers {
void register_fault_handlers();
void register_ask_handlers();
void register_system_handlers();
void register_dlq_handlers();
void register_actor_handlers();
void register_legacy_handler();
} // namespace handlers

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
    // Register OO-style handlers (they take priority in dispatch).
    handlers::register_fault_handlers();
    handlers::register_ask_handlers();
    handlers::register_system_handlers();
    handlers::register_dlq_handlers();
    handlers::register_actor_handlers();
    handlers::register_legacy_handler();
}

void CliHttpServerActor::dispatch_route(net::HTTPConnection* conn,
                                        net::HttpRequest&& req) {
    for (const auto& entry : HttpHandlerRegistry::instance().routes()) {
        if (entry.method != req.method)
            continue;
        req.path_params.clear();
        if (match_route_pattern(entry.pattern, req.path, req.path_params)) {
            entry.handler->handle(*this, *conn, std::move(req));
            return;
        }
    }
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
