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
#include <memory>
#include <string>

namespace hpactor {

class ActorSystem;

namespace net {
class EventLoop;
}

namespace process {

struct HealthHttpConfig {
    uint16_t port = 8089;
    std::string bind_address = "127.0.0.1";
};

/// \brief Minimal HTTP health-check endpoint server.
///
/// Runs on its own daemon thread and uses a dedicated EventLoop for
/// non-blocking I/O.  Responds to \c GET requests on \c /health/live,
/// \c /health/ready, and \c /health/startup with \c 200\ OK.
class HealthHttpServer : public DaemonActor {
  public:
    static constexpr const char* kActorTypeName = "HealthHttpServer";

    HealthHttpServer(ActorContext* ctx, ActorSystem& system,
                     const HealthHttpConfig& config);

    /// \brief Explicit destructor — defined in .cpp so that
    ///        \c unique_ptr<EventLoop> can own an incomplete type.
    ~HealthHttpServer() override;

    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;
    bool is_system_actor() const override {
        return true;
    }

  private:
    /// Build the HTTP response for \p path.
    std::string health_response(const std::string& path) const;

    /// Handler invoked by the EventLoop when the listen socket is readable.
    /// Accepts pending connections and processes each inline (the requests
    /// and responses are tiny, so non-blocking read/write on a
    /// freshly-accepted fd completes immediately under normal conditions).
    void on_listen_readable(int listen_fd);

    ActorSystem& system_;
    HealthHttpConfig config_;
    std::unique_ptr<net::EventLoop> health_loop_;
    int listen_fd_ = -1;
    bool running_ = true;
};

} // namespace process
} // namespace hpactor
