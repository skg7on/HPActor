// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <iostream>
#include <thread>

using namespace hpactor;

#define TEST(name) static void name()
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":"        \
                      << __LINE__ << '\n';                                     \
            std::abort();                                                      \
        }                                                                      \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

TEST(test_no_lifecycle_returns_null) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();
    CHECK(actor.get()->as_lifecycle() == nullptr);
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

TEST(test_lifecycle_actor_spawns_active) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<SimpleLifecycleActor>();
    auto* lc = actor.get()->as_lifecycle();
    CHECK(lc != nullptr);
    CHECK_EQ(lc->state(), LifecycleState::kActive);
    std::cout << "PASS: test_lifecycle_actor_spawns_active\n";
}

TEST(test_to_metadata_reports_lifecycle_state) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<SimpleLifecycleActor>();
    auto meta = actor.get()->to_metadata();
    CHECK_EQ(meta.state, "active");
    std::cout << "PASS: test_to_metadata_reports_lifecycle_state\n";
}

TEST(test_default_actor_to_metadata_unknown) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();
    auto meta = actor.get()->to_metadata();
    CHECK_EQ(meta.state, "unknown");
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
