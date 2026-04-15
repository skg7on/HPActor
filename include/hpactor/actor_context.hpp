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
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <vector>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorContext - execution context for actors
// -----------------------------------------------------------------------------
class ActorContext {
  public:
    explicit ActorContext(Actor owner);
    ~ActorContext();

    // Spawn child actors
    template <typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    template <typename T, typename... Args> T spawn(Args&&... args);

    // Send messages
    void send(const ActorAddress& target, MessageVariant msg);

    // Replies
    void reply(MessageVariant msg);
    void reply_with_error(error err);

    // Scheduled execution
    void schedule(std::chrono::milliseconds delay, MessageVariant msg);

    // Children management
    std::vector<Actor> children() const;
    void add_child(Actor child);
    void remove_child(Actor child);

    // Link management
    std::vector<ActorAddress> linked_actors() const;

    // Monitoring
    void monitor(const ActorAddress& target);

  private:
    Actor owner_;
    std::vector<Actor> children_;
    std::vector<ActorAddress> linked_;
    std::vector<ActorAddress> monitored_;
};

} // namespace hpactor