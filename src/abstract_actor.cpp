#include <hpactor/actor/abstract_actor.hpp>

namespace hpactor {

AbstractActor::AbstractActor(ActorId id, ActorType type, ActorSystem& sys)
    : id_(id), type_(type), system_(sys) {}

void AbstractActor::link_to(const ActorAddr& /*other*/) {
    // TODO: implement link mechanism
}

void AbstractActor::unlink_from(const ActorAddr& /*other*/) {
    // TODO: implement unlink mechanism
}

void AbstractActor::monitor(const ActorAddr& /*target*/) {
    // TODO: implement monitor mechanism
}

void AbstractActor::demonitor(const ActorAddr& /*target*/) {
    // TODO: implement demonitor mechanism
}

} // namespace hpactor
