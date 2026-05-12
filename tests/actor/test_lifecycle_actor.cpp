// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <iostream>

using namespace hpactor;

static void test_no_lifecycle_returns_null() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();
    assert(actor.get()->as_lifecycle() == nullptr);
    std::cout << "PASS: test_no_lifecycle_returns_null\n";
}

class SimpleLifecycleActor : public EventBasedActor, public LifecycleActor {
  public:
    SimpleLifecycleActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }
};

static void test_lifecycle_actor_spawns_active() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<SimpleLifecycleActor>();
    auto* lc = actor.get()->as_lifecycle();
    assert(lc != nullptr);
    assert(lc->state() == LifecycleState::kActive);
    std::cout << "PASS: test_lifecycle_actor_spawns_active\n";
}

static void test_to_metadata_reports_lifecycle_state() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<SimpleLifecycleActor>();
    auto meta = actor.get()->to_metadata();
    assert(meta.state == "active");
    std::cout << "PASS: test_to_metadata_reports_lifecycle_state\n";
}

static void test_default_actor_to_metadata_unknown() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();
    auto meta = actor.get()->to_metadata();
    assert(meta.state == "unknown");
    std::cout << "PASS: test_default_actor_to_metadata_unknown\n";
}

int main() {
    test_no_lifecycle_returns_null();
    test_lifecycle_actor_spawns_active();
    test_to_metadata_reports_lifecycle_state();
    test_default_actor_to_metadata_unknown();
    std::cout << "\nAll lifecycle actor integration tests passed.\n";
    return 0;
}
