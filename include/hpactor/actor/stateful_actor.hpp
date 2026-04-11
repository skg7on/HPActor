#pragma once

#include <hpactor/actor/event_based_actor.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// stateful_actor - event_based_actor with explicit state managed via a state
// class T
// -----------------------------------------------------------------------------
template<typename T>
class stateful_actor : public event_based_actor {
public:
    T& state() { return state_; }
    const T& state() const { return state_; }

protected:
    virtual Behavior make_behavior() = 0;

private:
    T state_;
};

} // namespace hpactor