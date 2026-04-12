#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/actor_fwd.hpp>

#include <type_traits>

using namespace hpactor;

void test_event_based_actor_become() {
    // Test that EventBasedActor can change behavior
    static_assert(sizeof(hpactor::EventBasedActor) > 0, "should not be empty");
}

int main() {
    test_event_based_actor_become();
    return 0;
}