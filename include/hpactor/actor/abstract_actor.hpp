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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <variant>

namespace hpactor {

// Forward declarations
class ActorSystem;

// -----------------------------------------------------------------------------
// MessageVariant - std::variant for all message types
// -----------------------------------------------------------------------------
struct down_msg {
    ActorAddress terminated_actor;
    error reason;
};

struct exit_msg {
    ActorAddress sender;
    error reason;
};

struct link_msg {
    ActorAddress target;
};

struct unlink_msg {
    ActorAddress target;
};

using MessageVariant = std::variant<
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg
    // ... user-defined types
>;

// -----------------------------------------------------------------------------
// AbstractActor - base class for all actors
// -----------------------------------------------------------------------------
class AbstractActor : public std::enable_shared_from_this<AbstractActor> {
public:
    virtual ~AbstractActor() = default;

    ActorId id() const { return id_; }
    ActorType type() const { return type_; }
    ActorAddress address() const { return address_; }
    ActorSystem& system() { return system_; }
    const ActorSystem& system() const { return system_; }

    // Set actor address (called by ActorSystem during spawn)
    void set_address(ActorAddress addr) { address_ = addr; }

    // Linking - death sharing
    void link_to(const ActorAddr& other);
    void unlink_from(const ActorAddr& other);

    // Monitoring - receive down messages
    void monitor(const ActorAddr& target);
    void demonitor(const ActorAddr& target);

    // Receive message (called by scheduler)
    virtual void receive(MessageVariant&& msg) = 0;

protected:
    AbstractActor(ActorId id, ActorType type, ActorSystem& sys);

private:
    ActorId id_;
    ActorType type_;
    ActorSystem& system_;
    ActorAddress address_;
};

} // namespace hpactor
