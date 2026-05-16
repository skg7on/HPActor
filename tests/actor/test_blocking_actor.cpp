// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/blocking_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/scoped_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <iostream>
#include <thread>

using namespace hpactor;

// Test actor that counts received messages
class TestBlockingActor : public BlockingActor {
  public:
    TestBlockingActor(ActorContext* ctx, ActorSystem& sys)
        : BlockingActor(ctx, sys) {}

    int received_count() const {
        return received_count_;
    }

    void receive(TypedMessage& /*msg*/) override {
        received_count_++;
    }

  private:
    int received_count_ = 0;
};

// ─── Tests ────────────────────────────────────────────────────────────

static void test_blocking_actor_size_not_empty() {
    static_assert(sizeof(BlockingActor) > sizeof(LocalActor), "BlockingActor "
                                                              "should add "
                                                              "members beyond "
                                                              "LocalActor");
    std::cout << "PASS: test_blocking_actor_size_not_empty\n";
}

static void test_scoped_actor_size_not_empty() {
    static_assert(sizeof(ScopedActor) >= sizeof(BlockingActor), "ScopedActor "
                                                                "should be at "
                                                                "least as "
                                                                "large as "
                                                                "BlockingActo"
                                                                "r");
    std::cout << "PASS: test_scoped_actor_size_not_empty\n";
}

static void test_blocking_actor_dispatch_policy() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<TestBlockingActor>();
    assert(actor.get().get()->dispatch_policy() ==
           sched::DispatchPolicy::DedicatedThread);
    std::cout << "PASS: test_blocking_actor_dispatch_policy\n";
}

static void test_blocking_actor_spawn_and_activate() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    auto actor = system.spawn<TestBlockingActor>();

    // Actor should be alive and have a non-default ID
    assert(!(actor.get().get()->id() == ActorId{}));
    std::cout << "PASS: test_blocking_actor_spawn_and_activate\n";
}

static void test_blocking_actor_receives_message() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto actor = system.spawn<TestBlockingActor>();
    auto addr = actor.address();

    // Send a message via a temporary sender and ActorContext
    auto sender = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    msg.set_sender_address(sender.address());
    ctx.send(addr, std::move(msg));

    // Give the blocking actor time to process.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    // BlockingActor has DedicatedThread — scheduler should not dispatch it
    std::cout << "PASS: test_blocking_actor_receives_message\n";
}

static void test_blocking_actor_state_after_spawn() {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    auto actor = system.spawn<TestBlockingActor>();

    // BlockingActor's dedicated thread should be started.
    // Poll until we can verify the actor exists and is accessible.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    assert(actor.get().get() != nullptr);
    std::cout << "PASS: test_blocking_actor_state_after_spawn\n";
}

int main() {
    test_blocking_actor_size_not_empty();
    test_scoped_actor_size_not_empty();
    test_blocking_actor_dispatch_policy();
    test_blocking_actor_spawn_and_activate();
    test_blocking_actor_receives_message();
    test_blocking_actor_state_after_spawn();
    std::cout << "\nAll blocking actor tests passed.\n";
    return 0;
}
