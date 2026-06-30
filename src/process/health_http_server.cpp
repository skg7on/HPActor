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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_gateway.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/process/health_check.hpp>
#include <hpactor/process/health_http_server.hpp>

#include <cstdio>

namespace hpactor::process {

HealthHttpServer::HealthHttpServer(ActorContext* ctx, ActorSystem& system,
                                   const HealthHttpConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      gateway_(std::make_unique<net::HTTPGateway>()) {
    (void)system_; // may be unused when health_state_ is wired; kept for API
                   // compat
}

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

    // Serve health endpoints reflecting actual system state when
    // a HealthState is attached; otherwise unconditionally return 200.
    gateway_->set_request_handler(
        [this](net::HTTPConnection* conn, net::HttpRequest&& req) {
            (void)req;

            HealthStatus status = HealthStatus::Healthy;
            std::string content_type;
            std::string body;

            if (health_state_) {
                status = health_state_->overall_status();

                if (status != HealthStatus::Healthy) {
                    body = format_health_json(*health_state_);
                    content_type = "application/json";
                } else {
                    body = "OK";
                }
            } else {
                body = "OK";
            }

            net::HttpStatusCode http_code = net::HttpStatusCode::OK;
            switch (status) {
                case HealthStatus::Healthy:
                case HealthStatus::Degraded:
                    http_code = net::HttpStatusCode::OK;
                    break;
                case HealthStatus::Unhealthy:
                    http_code = net::HttpStatusCode::ServiceUnavailable;
                    break;
            }

            StreamBuffer body_buf(
                reinterpret_cast<const uint8_t*>(body.data()),
                reinterpret_cast<const uint8_t*>(body.data() + body.size()));

            std::vector<net::HttpHeader> headers;
            if (!content_type.empty()) {
                headers.push_back({"content-type", content_type});
            }

            conn->send_response(http_code, headers, body_buf);
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

std::string HealthHttpServer::format_health_json(const HealthState& state) {
    std::string json;
    json.reserve(512);

    const char* status_str = "healthy";
    switch (state.overall_status()) {
        case HealthStatus::Healthy:
            status_str = "healthy";
            break;
        case HealthStatus::Degraded:
            status_str = "degraded";
            break;
        case HealthStatus::Unhealthy:
            status_str = "unhealthy";
            break;
    }

    json += "{\"status\":\"";
    json += status_str;
    json += "\",\"checks\":[";

    const auto& details = state.details();
    bool first = true;
    for (const auto& d : details) {
        if (!first)
            json += ",";
        first = false;
        json += "{\"name\":\"";
        json += d.check_name;
        json += "\",\"status\":\"";
        switch (d.status) {
            case HealthStatus::Healthy:
                json += "healthy";
                break;
            case HealthStatus::Degraded:
                json += "degraded";
                break;
            case HealthStatus::Unhealthy:
                json += "unhealthy";
                break;
        }
        json += "\",\"reason\":\"";
        // Simple escaping: only escape backslash and double-quote.
        for (char c : d.reason) {
            if (c == '\\' || c == '"')
                json += '\\';
            json += c;
        }
        json += "\"}";
    }

    json += "]}";
    return json;
}

} // namespace hpactor::process
