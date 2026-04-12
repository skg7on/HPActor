#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_system.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/serialization.hpp>

namespace hpactor {

SpawnReceiver::SpawnReceiver(ActorSystem& sys,
                            ActorTypeRegistry& registry,
                            net::Transport* transport)
    : EventBasedActor(nullptr, sys), registry_(registry), transport_(transport) {}

Behavior SpawnReceiver::make_behavior() {
    return {
        {[this](const SpawnRequest& req, uint64_t message_id) {
            handle_spawn_request(req, message_id);
        }}
    };
}

void SpawnReceiver::handle_spawn_request(const SpawnRequest& req, uint64_t message_id) {
    SpawnResponse response;

    auto result = registry_.spawn(system(), req.actor_type_name);
    if (result) {
        response.actor_addr = result.value();
        response.error_code = spawn_errors::success;
    } else {
        response.error_code = result.error().code();
    }

    // TODO: Send response back via transport using Frame.message_id to route
    // This requires transport to support reply routing
    (void)message_id;
}

} // namespace hpactor