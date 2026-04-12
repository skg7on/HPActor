#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/spawn.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// SpawnReceiver - system actor handling remote spawn requests
// -----------------------------------------------------------------------------
// Receives SpawnRequest messages, creates actors via ActorTypeRegistry,
// and sends SpawnResponse back to the caller via Transport.
class SpawnReceiver : public EventBasedActor {
public:
    SpawnReceiver(ActorSystem& sys, ActorTypeRegistry& registry, net::Transport* transport);

    Behavior make_behavior() override;

private:
    void handle_spawn_request(const SpawnRequest& req);

    ActorTypeRegistry& registry_;
    net::Transport* transport_;  // non-owning
};

} // namespace hpactor