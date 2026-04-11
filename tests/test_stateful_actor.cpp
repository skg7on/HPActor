#include <hpactor/actor/stateful_actor.hpp>

using namespace hpactor;

struct counter_state {
    int value = 0;
};

void test_stateful_actor_state_access() {
    static_assert(sizeof(hpactor::StatefulActor<counter_state>) > 0, "should not be empty");
}

int main() {
    test_stateful_actor_state_access();
    return 0;
}