#include <cassert>
#include <cstdio>
#include <hpactor/actor/scoped_actor.hpp>
#include <hpactor/core/actor_system.hpp>

using namespace hpactor;

void test_construct_destruct() {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);
    {
        ScopedActor actor(sys);
        // Verify actor has a reference to the ActorSystem
        assert(&actor.system() == &sys);
    }
    printf("  PASSED test_construct_destruct\n");
}

int main() {
    printf("ScopedActor tests:\n");
    test_construct_destruct();
    printf("All ScopedActor tests PASSED\n");
    return 0;
}
