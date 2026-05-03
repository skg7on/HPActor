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

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/core/actor_system.hpp>

namespace hpactor {

DaemonActor::DaemonActor(ActorContext* ctx, ActorSystem& sys)
    : EventBasedActor(ctx, sys) {}

DaemonActor::~DaemonActor() {
    running_.store(false, std::memory_order_release);
    if (daemon_thread_.joinable()) {
        daemon_thread_.join();
    }
}

void DaemonActor::on_activate() {
    EventBasedActor::on_activate();

    running_.store(true, std::memory_order_release);
    daemon_thread_ = std::thread(&DaemonActor::daemon_loop, this);
}

void DaemonActor::on_deactivate() {
    running_.store(false, std::memory_order_release);
    if (daemon_thread_.joinable()) {
        daemon_thread_.join();
    }
    EventBasedActor::on_deactivate();
}

void DaemonActor::daemon_loop() {
    on_daemon_start();
    while (running_.load(std::memory_order_acquire)) {
        if (!run_once()) break;
    }
    on_daemon_stop();
    running_.store(false, std::memory_order_release);
}

} // namespace hpactor
