#include <hpactor/actor_type_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/behavior.hpp>

#include <iostream>
#include <cassert>

using namespace hpactor;

// Test actor for registration - must be default constructible for spawn
class TestActor : public EventBasedActor {
public:
    TestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return {};
    }
};

void test_register_and_lookup() {
    ActorTypeRegistry registry;
    registry.register_type<TestActor>("test-actor");

    assert(registry.has("test-actor"));
    assert(!registry.has("non-existent"));
    assert(registry.type_id("test-actor") != ActorType{0});
}

void test_spawn_unknown_type() {
    ActorTypeRegistry registry;
    Config config;
    ActorSystem system{config};

    auto result = registry.spawn(system, "non-existent");
    assert(!result.has_value());
    assert(result.error().code() == spawn_errors::unknown_type);
}

void test_spawn_valid_type() {
    ActorTypeRegistry registry;
    Config config;
    ActorSystem system{config};

    registry.register_type<TestActor>("test-actor");
    auto result = registry.spawn(system, "test-actor");
    assert(result.has_value());
    assert(result.value().node_id == LocalNodeId);
}

int main() {
    test_register_and_lookup();
    test_spawn_unknown_type();
    test_spawn_valid_type();
    std::cout << "All ActorTypeRegistry tests passed\n";
    return 0;
}