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
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <vector>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorContext - execution context for actors
// -----------------------------------------------------------------------------
class ActorContext {
  public:
    explicit ActorContext(Actor owner, ActorSystem* system = nullptr);
    ~ActorContext();

    // Set the system reference (used when owner is not set)
    void set_system(ActorSystem* system) { system_ = system; }

    // Spawn child actors
    template <typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    template <typename T, typename... Args> T spawn(Args&&... args);

    // Send messages
    void send(const ActorAddress& target, MessageVariant msg);

    // Send message with priority and deadline for scheduling
    // priority: 0-3 (0 = highest)
    // deadline_ns: absolute deadline in nanoseconds (INT64_MAX = no deadline)
    void send_with_priority(const ActorAddress& target, MessageVariant msg,
                          uint8_t priority, int64_t deadline_ns);

    // Replies
    void reply(MessageVariant msg);
    void reply_with_error(error err);

    // Scheduled execution
    void schedule(std::chrono::milliseconds delay, MessageVariant msg);

    // Children management
    std::vector<Actor> children() const;
    void add_child(Actor child);
    void remove_child(Actor child);

    // Remote child management
    void add_remote_child(ActorRef child);
    std::vector<ActorRef> remote_children() const;

    // Link management
    std::vector<ActorAddress> linked_actors() const;

    // Monitoring
    void monitor(const ActorAddress& target);

    // RPC calls (for non-actor threads only)
    // Note: caller serializes request and deserializes response
    RpcFuture<bytes> rpc(const ActorAddress& target,
                         const bytes& encoded_request,
                         std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(5000));

  private:
    Actor owner_;
    ActorSystem* system_ = nullptr;
    std::vector<Actor> children_;
    std::vector<ActorRef> remote_children_;
    std::vector<ActorAddress> linked_;
    std::vector<ActorAddress> monitored_;
};

} // namespace hpactor