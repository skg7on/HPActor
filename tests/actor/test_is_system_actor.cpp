// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <iostream>

using namespace hpactor;

// Test that a regular EventBasedActor (user actor) is not a system actor.
static void test_user_actor_is_not_system() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();
    assert(!actor.get()->is_system_actor());
    std::cout << "PASS: test_user_actor_is_not_system\n";
}

// Test-only system actor that overrides is_system_actor() to return true.
class TestSystemActor : public EventBasedActor {
  public:
    TestSystemActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    bool is_system_actor() const override {
        return true;
    }
};

// Test that an actor overriding is_system_actor() to true is identified as a
// system actor.
static void test_system_actors_return_true() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<TestSystemActor>();
    assert(actor.get()->is_system_actor());
    std::cout << "PASS: test_system_actors_return_true\n";
}

int main() {
    test_user_actor_is_not_system();
    test_system_actors_return_true();
    std::cout << "\nAll is_system_actor tests passed.\n";
    return 0;
}
