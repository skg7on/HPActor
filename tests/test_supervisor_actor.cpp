#include <hpactor/supervision/supervision.hpp>
#include <cassert>

void test_supervisor_actor_interface() {
    static_assert(sizeof(hpactor::supervisor_actor) > 0, "should not be empty");
}

int main() {
    test_supervisor_actor_interface();
    return 0;
}