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
#include <hpactor/actor/message.hpp>

#include <memory>
#include <string>
#include <variant>

namespace hpactor {

// Forward declarations
class ActorSystem;
namespace net {
enum class OpType : uint32_t;
}  // namespace net
namespace sched {
class IScheduler;
}  // namespace sched
namespace mailbox {
template<typename T>
class MPSCActorMailbox;
}  // namespace mailbox

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

// completion_msg carries I/O completion data from EventLoop to actors
struct completion_msg {
    ActorId actor;       // target actor that initiated the operation
    net::OpType type;    // Send, Recv, Accept, Connect, TimerFired, RecvFrom, SendTo
    int fd;              // file descriptor
    int result;          // bytes transferred (>= 0) or -errno on failure
    uint64_t user_data;  // original user data from the async operation
};

// -----------------------------------------------------------------------------
// User message types for examples and testing
// -----------------------------------------------------------------------------
struct ping_msg {
    ActorAddress from;
    int sequence;
};

struct pong_msg {
    ActorAddress from;
    int sequence;
};

struct stop_msg {};

struct start_msg {
    int count;
};

struct work_msg {
    int value;
};

struct result_msg {
    int value;
};

struct status_msg {
    std::string label;
};

// -----------------------------------------------------------------------------
// MessageVariant - std::variant for all message types
// -----------------------------------------------------------------------------
using MessageVariant = std::variant<
    completion_msg,
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg,
    // User-defined message types
    ping_msg,
    pong_msg,
    stop_msg,
    start_msg,
    work_msg,
    result_msg,
    status_msg
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

    // Set scheduler and mailbox (called by ActorSystem during spawn)
    // Default implementations in .cpp; EventBasedActor overrides these
    virtual void set_scheduler(sched::IScheduler* scheduler);
    virtual void set_mailbox(mailbox::MPSCActorMailbox<Message<MessageVariant>>* mailbox);

    // Linking - death sharing
    void link_to(const ActorAddr& other);
    void unlink_from(const ActorAddr& other);

    // Monitoring - receive down messages
    void monitor(const ActorAddr& target);
    void demonitor(const ActorAddr& target);

    // Receive message (called by scheduler)
    virtual void receive(MessageVariant&& msg) = 0;

    // Type query for safe downcasting without RTTI
    virtual bool is_event_based_actor() const { return false; }

protected:
    AbstractActor(ActorId id, ActorType type, ActorSystem& sys);

private:
    ActorId id_;
    ActorType type_;
    ActorSystem& system_;
    ActorAddress address_;
};

} // namespace hpactor
