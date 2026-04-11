#pragma once

#include <hpactor/actor/blocking_actor.hpp>

namespace hpactor {

// Forward declaration
class ActorSystem;

// -----------------------------------------------------------------------------
// scoped_actor - blocking actor for non-actor contexts (e.g., main function)
// -----------------------------------------------------------------------------
class scoped_actor : public blocking_actor {
public:
    explicit scoped_actor(ActorSystem& sys);
    ~scoped_actor();

    template<typename T>
    T receive();
};

} // namespace hpactor