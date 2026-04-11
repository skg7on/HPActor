// ActorProxy implementation - see actor_proxy.hpp

#include <hpactor/ref/actor_proxy.hpp>

namespace hpactor {

ActorProxy::ActorProxy(ActorAddress address, net::Transport* transport)
    : address_(address), transport_(transport) {}

void ActorProxy::send(const ActorAddress& /*target*/, MessageVariant /*msg*/) {
    // Remote send via transport - will be implemented in Phase 2
    // when serialization and transport are available
}

} // namespace hpactor
