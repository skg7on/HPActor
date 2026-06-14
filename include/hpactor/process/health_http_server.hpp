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

#include <cstdint>
#include <hpactor/actor/daemon_actor.hpp>
#include <string>

namespace hpactor {

class ActorSystem;

namespace process {

struct HealthHttpConfig {
    uint16_t port = 8089;
    std::string bind_address = "127.0.0.1";
};

class HealthHttpServer : public DaemonActor {
  public:
    static constexpr const char* kActorTypeName = "HealthHttpServer";

    HealthHttpServer(ActorContext* ctx, ActorSystem& system,
                     const HealthHttpConfig& config);

    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override {
        return true;
    }

  private:
    void handle_request(int client_fd);
    std::string health_response(const std::string& path) const;
    int portable_accept(int listen_fd);

    ActorSystem& system_;
    HealthHttpConfig config_;
    int listen_fd_ = -1;
    bool running_ = true;
};

} // namespace process
} // namespace hpactor
