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

#include <chrono>
#include <hpactor/actor/event_based_actor.hpp>

namespace hpactor {

class ActorSystem;

namespace process {

struct WatchdogCheck {};

class WatchdogActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "WatchdogActor";

    WatchdogActor(ActorContext* ctx, ActorSystem& system,
                  std::chrono::milliseconds interval);

    Behavior make_behavior() override;
    bool is_system_actor() const override {
        return true;
    }

  private:
    void on_check();
    bool is_system_healthy() const;

    std::chrono::milliseconds interval_;
    ActorSystem& system_;
};

} // namespace process
} // namespace hpactor
