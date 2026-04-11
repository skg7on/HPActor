#include <cassert>
#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>

void test_actor_default() {
    hpactor::Actor actor;
    assert(!actor);
    assert(actor.id().value() == 0);
}

void test_actor_conversion_to_address() {
    hpactor::Actor actor;
    hpactor::ActorAddress addr = actor.address();
    assert(!addr);
}

int main() {
    test_actor_default();
    test_actor_conversion_to_address();

    return 0;
}
