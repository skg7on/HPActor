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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_context.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// LocalActor - base class for actors with access to ActorContext
// -----------------------------------------------------------------------------
class LocalActor : public AbstractActor {
  public:
    ActorContext* context() {
        return ctx_;
    }
    ActorSystem& home_system() {
        return system();
    }

    // Set the actor context (called by ActorSystem during spawn)
    void set_context(ActorContext* ctx) {
        ctx_ = ctx;
    }

  protected:
    LocalActor(ActorContext* ctx, ActorSystem& sys);
    LocalActor(ActorId id, ActorContext* ctx, ActorSystem& sys);

    ActorContext* actor_context() override {
        return ctx_;
    }

  public:
    virtual void on_activate() {}
    virtual void on_deactivate() {}

  private:
    ActorContext* ctx_ = nullptr;
};

} // namespace hpactor