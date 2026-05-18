#include <cassert>
#include <cstdio>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/core/actor_system.hpp>

using namespace hpactor;

// Concrete subclass that exposes LocalActor's protected constructors
// and satisfies the pure virtual receive() from AbstractActor.
class TestLocalActor : public LocalActor {
  public:
    TestLocalActor(ActorContext* ctx, ActorSystem& sys)
        : LocalActor(ctx, sys) {}
    TestLocalActor(ActorId id, ActorContext* ctx, ActorSystem& sys)
        : LocalActor(id, ctx, sys) {}

    void receive(TypedMessage& /*msg*/) override {}
};

void test_two_arg_constructor() {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);
    TestLocalActor actor(nullptr, sys);
    // Verify actor has a reference to the ActorSystem
    assert(&actor.system() == &sys);
    printf("  PASSED test_two_arg_constructor\n");
}

void test_three_arg_constructor() {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);
    TestLocalActor actor(ActorId{42}, nullptr, sys);
    assert(&actor.system() == &sys);
    // Verify the custom ActorId was assigned
    assert(actor.id().value() == 42);
    printf("  PASSED test_three_arg_constructor\n");
}

int main() {
    printf("LocalActor tests:\n");
    test_two_arg_constructor();
    test_three_arg_constructor();
    printf("All LocalActor tests PASSED\n");
    return 0;
}
