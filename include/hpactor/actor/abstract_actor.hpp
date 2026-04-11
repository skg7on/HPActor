#pragma once

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types.hpp>

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
// abstract_actor - base class for all actors
// -----------------------------------------------------------------------------
class abstract_actor : public std::enable_shared_from_this<abstract_actor> {
public:
    virtual ~abstract_actor() = default;

    ActorId id() const { return id_; }
    ActorType type() const { return type_; }
    ActorAddress address() const { return address_; }
    ActorSystem& system() { return system_; }
    const ActorSystem& system() const { return system_; }

    // Linking - death sharing
    void link_to(const ActorAddr& other);
    void unlink_from(const ActorAddr& other);

    // Monitoring - receive down messages
    void monitor(const ActorAddr& target);
    void demonitor(const ActorAddr& target);

    // Receive message (called by scheduler)
    virtual void receive(MessageVariant&& msg) = 0;

protected:
    abstract_actor(ActorId id, ActorType type, ActorSystem& sys);

private:
    ActorId id_;
    ActorType type_;
    ActorSystem& system_;
    ActorAddress address_;
};

} // namespace hpactor
