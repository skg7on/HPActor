#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/types/serialization.hpp>

namespace hpactor {

SpawnReceiver::SpawnReceiver(ActorSystem& sys,
                            ActorTypeRegistry& registry,
                            net::Transport* transport)
    : EventBasedActor(nullptr, sys), registry_(registry), transport_(transport) {}

Behavior SpawnReceiver::make_behavior() {
    return Behavior{[this](MessageVariant&& msg) {
        std::visit([this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, SpawnRequest>) {
                handle_spawn_request(m);
            }
        }, std::move(msg));
    }};
}

void SpawnReceiver::handle_spawn_request(const SpawnRequest& req) {
    SpawnResponse response;

    auto result = registry_.spawn(system(), req.actor_type_name);
    if (result.has_value()) {
        response.actor_addr = result.value();
        response.error_code = spawn_errors::success;
    } else {
        response.error_code = result.error().code();
    }

    // TODO: Send response back via transport using Frame.message_id to route
    // This requires transport to support reply routing
    (void)response;
    (void)transport_;
}

} // namespace hpactor