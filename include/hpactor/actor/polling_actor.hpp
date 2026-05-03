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

#include <hpactor/actor/daemon_actor.hpp>

namespace hpactor {

class PollingActor : public DaemonActor {
  public:
    PollingActor(ActorContext* ctx, ActorSystem& sys, int cpu_core = -1)
        : DaemonActor(ctx, sys) {
        if (cpu_core >= 0) set_cpu_affinity(cpu_core);
    }

    void set_poll_budget(uint32_t max_events) { poll_budget_ = max_events; }
    uint32_t poll_budget() const { return poll_budget_; }

  protected:
    uint32_t poll_budget_ = 64;
};

} // namespace hpactor
