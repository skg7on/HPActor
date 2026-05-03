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

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/core/mailbox.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace hpactor {

// -----------------------------------------------------------------------------
// BlockingActor - actor that runs in its own thread with blocking receive
// -----------------------------------------------------------------------------
class BlockingActor : public LocalActor {
  public:
    template <typename... Handlers> void receive(Handlers&&... handlers);

    template <typename T> void receive_for(T& begin, T end);

    template <typename... Actors>
    void wait_for(ActorAddr first, Actors&&... rest);

    void await_all_other_actors_done();

    const error& fail_state() const {
        return fail_state_;
    }
    void fail_state(error e) {
        fail_state_ = e;
    }

    void on_activate() override;
    void on_deactivate() override;

  protected:
    BlockingActor(ActorContext* ctx, ActorSystem& sys);
    BlockingActor(ActorId id, ActorContext* ctx, ActorSystem& sys);

  private:
    error fail_state_;
};

} // namespace hpactor