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