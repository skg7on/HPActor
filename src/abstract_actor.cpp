#include <hpactor/actor/abstract_actor.hpp>

namespace hpactor {

abstract_actor::abstract_actor(ActorId id, ActorType type, ActorSystem& sys)
    : id_(id), type_(type), system_(sys) {}

void abstract_actor::link_to(const ActorAddr& /*other*/) {
    // TODO: implement link mechanism
}

void abstract_actor::unlink_from(const ActorAddr& /*other*/) {
    // TODO: implement unlink mechanism
}

void abstract_actor::monitor(const ActorAddr& /*target*/) {
    // TODO: implement monitor mechanism
}

void abstract_actor::demonitor(const ActorAddr& /*target*/) {
    // TODO: implement demonitor mechanism
}

} // namespace hpactor
