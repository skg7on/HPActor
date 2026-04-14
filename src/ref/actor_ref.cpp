// Actor reference implementation - see actor_ref.hpp

#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_ref.hpp>

namespace hpactor {

void ActorRef::send(const ActorAddress& target, MessageVariant msg) {
    if (is_local()) {
        Actor* actor = get_actor();
        if (actor) {
            actor->get()->system().deliver_local(target.id, std::move(msg));
        }
    } else {
        // Remote send via proxy - not implemented until Phase 2
        // ActorProxy* proxy = get_proxy();
    }
}

} // namespace hpactor
