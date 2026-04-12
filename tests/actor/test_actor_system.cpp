#include <hpactor/actor_system.hpp>

#include <cassert>
#include <type_traits>

using namespace hpactor;

void test_actor_system_default_construct() {
    static_assert(!std::is_copy_constructible_v<hpactor::ActorSystem>);
}

int main() {
    test_actor_system_default_construct();
    return 0;
}