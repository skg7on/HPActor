#include <hpactor/actor/blocking_actor.hpp>
#include <hpactor/actor/scoped_actor.hpp>
#include <hpactor/actor/actor_fwd.hpp>

#include <type_traits>

using namespace hpactor;

void test_blocking_actor_interface() {
    static_assert(sizeof(hpactor::blocking_actor) > 0, "should not be empty");
}

void test_scoped_actor_interface() {
    static_assert(sizeof(hpactor::scoped_actor) > 0, "should not be empty");
}

int main() {
    test_blocking_actor_interface();
    test_scoped_actor_interface();
    return 0;
}