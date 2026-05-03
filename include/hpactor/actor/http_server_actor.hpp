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

#pragma once

#include <hpactor/actor/external_msg_gateway.hpp>

#include <string>
#include <vector>

namespace hpactor {

// Legacy skeleton for ExternalMsgGatewayActor-based HTTP gateway.
// Superseded by net::HTTPGatewayActor (DaemonActor-based, in hpactor/net/http_gateway.hpp).
class HTTPGatewayActor : public ExternalMsgGatewayActor {
  public:
    HTTPGatewayActor(ActorContext* ctx, ActorSystem& sys,
                    const std::string& bind_addr, uint16_t port)
        : ExternalMsgGatewayActor(ctx, sys),
          bind_addr_(bind_addr), port_(port) {}

    void get(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"GET", path, handler});
    }

    void post(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"POST", path, handler});
    }

    void put(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"PUT", path, handler});
    }

    void del(const std::string& path, ActorAddr handler) {
        http_routes_.push_back({"DELETE", path, handler});
    }

    bool run_once() override {
        // TODO: accept connections, parse HTTP, dispatch to actors.
        return true;
    }

  protected:
    struct RouteEntry {
        std::string method;
        std::string path;
        ActorAddr handler;
    };

    std::vector<RouteEntry> http_routes_;
    std::string bind_addr_;
    uint16_t port_;
};

} // namespace hpactor
