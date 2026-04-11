#include <hpactor/supervision/supervision.hpp>
#include <cassert>

void test_self_supervising_actor_interface() {
    static_assert(sizeof(hpactor::self_supervising_actor) > 0, "should not be empty");
}

int main() {
    test_self_supervising_actor_interface();
    return 0;
}