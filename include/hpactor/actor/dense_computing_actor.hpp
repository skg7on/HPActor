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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/sched/dispatch_policy.hpp>

namespace hpactor {

class DenseComputingActor : public EventBasedActor {
  public:
    DenseComputingActor(ActorContext* ctx, ActorSystem& sys,
                        uint32_t pool_size = 1)
        : EventBasedActor(ctx, sys), pool_size_(pool_size) {}

    sched::DispatchPolicy dispatch_policy() const override {
        return sched::DispatchPolicy::DedicatedPool;
    }

    sched::DispatchHints dispatch_hints() const override {
        sched::DispatchHints h;
        h.pool_size = pool_size_;
        return h;
    }

    uint32_t pool_size() const { return pool_size_; }

  protected:
    uint32_t pool_size_;
};

} // namespace hpactor
