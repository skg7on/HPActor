#pragma once

#include <hpactor/actor/blocking_actor.hpp>

namespace hpactor {

// Forward declaration
class ActorSystem;

// -----------------------------------------------------------------------------
// ScopedActor - blocking actor for non-actor contexts (e.g., main function)
// -----------------------------------------------------------------------------
class ScopedActor : public BlockingActor {
  public:
    explicit ScopedActor(ActorSystem& sys);
    ~ScopedActor();

    template <typename T> T receive();
};

} // namespace hpactor