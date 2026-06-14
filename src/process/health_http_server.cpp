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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_gateway.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/process/health_http_server.hpp>

#include <cstdio>

namespace hpactor::process {

HealthHttpServer::HealthHttpServer(ActorContext* ctx, ActorSystem& system,
                                   const HealthHttpConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      gateway_(std::make_unique<net::HTTPGateway>()) {}

HealthHttpServer::~HealthHttpServer() = default;

void HealthHttpServer::on_daemon_start() {
    // Start listening via the HTTPGateway.  It creates a TcpAcceptor,
    // initialises its EventLoop backend, and begins accepting connections.
    if (!gateway_->listen(config_.port, config_.bind_address)) {
        std::fprintf(stderr, "HealthHttpServer: failed to listen on %s:%u\n",
                     config_.bind_address.c_str(),
                     static_cast<unsigned>(config_.port));
        listen_ok_ = false;
        return;
    }
    listen_ok_ = true;

    // All paths return 200 OK.  HTTPConnection (via llhttp) has already
    // parsed the request, so req.path is exactly the URL path.
    gateway_->set_request_handler(
        [this](net::HTTPConnection* conn, net::HttpRequest&& req) {
            (void)req;
            (void)system_;
            std::string body = "OK";
            StreamBuffer body_buf(
                reinterpret_cast<const uint8_t*>(body.data()),
                reinterpret_cast<const uint8_t*>(body.data() + body.size()));
            conn->send_response(net::HttpStatusCode::OK, {}, body_buf);
        });

    // Optionally cap connections (health checks are lightweight, but
    // keep a reasonable bound).
    gateway_->set_max_connections(64);
}

bool HealthHttpServer::run_once() {
    // If the listen() call failed in on_daemon_start(), exit the daemon
    // loop immediately rather than spinning forever.
    if (!listen_ok_)
        return false;

    // Delegate to HTTPGateway's event loop poll — same pattern as
    // HTTPGatewayActor: wait(100ms) + process_completions().
    gateway_->run_once();
    return true;
}

void HealthHttpServer::on_daemon_stop() {
    gateway_->stop();
}

} // namespace hpactor::process
