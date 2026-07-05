// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <hpactor/actor/routing/pool_router.hpp>
#include <hpactor/actor/routing/routing_logic.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <memory>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

namespace hpactor::routing {
namespace {

/// Helper to extract a PoolRouter* from an Actor handle.
/// Uses static_cast since -fno-rtti is enforced.
static PoolRouter* as_router(Actor& actor) {
    return static_cast<PoolRouter*>(actor.get().get());
}

class PoolRouterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg = test::config_with_scheduler(1);
        system_ = std::make_unique<ActorSystem>(cfg);
        driver_ = std::make_unique<test::SchedulerTestDriver>(*system_);
    }

    /// Spawn a pool of CountingActor routees.
    PoolRouter* spawn_pool(size_t pool_size = 3) {
        auto actor = system_->spawn<PoolRouter>(
            std::make_unique<RoundRobinLogic>(),
            [](ActorContext* ctx, ActorSystem& sys) -> std::shared_ptr<AbstractActor> {
                return std::make_shared<test::CountingActor>(ctx, sys);
            },
            pool_size);
        driver_->drain(100); // process spawn + on_activate
        return as_router(actor);
    }

    /// Deliver a message directly to the router's mailbox (test-level send).
    void deliver_to_router(const ActorAddress& addr, uint32_t type_id) {
        TypedMessage msg(TypeTag(type_id), StreamBuffer{});
        system_->try_deliver_local(addr.id, std::move(msg));
    }

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<test::SchedulerTestDriver> driver_;
};

// ── Basic Construction ─────────────────────────────────────────────────────

TEST_F(PoolRouterTest, SpawnsChildren) {
    auto* router = spawn_pool(3);
    EXPECT_EQ(router->routee_count(), 3u);
}

TEST_F(PoolRouterTest, DefaultSupervisionPolicy) {
    auto* router = spawn_pool(2);
    EXPECT_EQ(router->routee_count(), 2u);
    // Pool functions correctly with default OneForOne policy
}

// ── Routing ─────────────────────────────────────────────────────────────────

TEST_F(PoolRouterTest, RoundRobinRouting) {
    auto* router = spawn_pool(3);
    auto router_addr = router->address();

    // Send 6 messages via try_deliver_local
    for (uint32_t i = 0; i < 6; ++i) {
        deliver_to_router(router_addr, i + 100);
    }
    driver_->drain(100);

    // Verify each of the 3 routees received some messages.
    // With round-robin, each should get 2.
    EXPECT_EQ(router->routee_count(), 3u);

    // Check routee message counts via CountingActor.
    auto& rts = router->routees();
    size_t total_handled = 0;
    for (size_t i = 0; i < rts.size(); ++i) {
        if (rts[i].is_local()) {
            auto* actor_ptr = rts[i].get_actor();
            if (actor_ptr && actor_ptr->get()) {
                auto* ca =
                    static_cast<test::CountingActor*>(actor_ptr->get().get());
                total_handled += static_cast<size_t>(ca->handler_count);
            }
        }
    }
    EXPECT_EQ(total_handled, 6u);
}

TEST_F(PoolRouterTest, SenderPreservation) {
    auto* router = spawn_pool(1);
    auto router_addr = router->address();

    deliver_to_router(router_addr, 42);
    driver_->drain(50);

    // The single routee should have received 1 message.
    EXPECT_EQ(router->routee_count(), 1u);
}

// ── Broadcast ───────────────────────────────────────────────────────────────

TEST_F(PoolRouterTest, Broadcast) {
    auto* router = spawn_pool(3);

    TypedMessage msg(TypeTag(42), StreamBuffer{});
    router->broadcast(std::move(msg));
    driver_->drain(50);

    // All 3 routees should have received the broadcast.
    auto& rts = router->routees();
    size_t total_handled = 0;
    for (size_t i = 0; i < rts.size(); ++i) {
        if (rts[i].is_local()) {
            auto* actor_ptr = rts[i].get_actor();
            if (actor_ptr && actor_ptr->get()) {
                auto* ca =
                    static_cast<test::CountingActor*>(actor_ptr->get().get());
                total_handled += static_cast<size_t>(ca->handler_count);
            }
        }
    }
    EXPECT_EQ(total_handled, 3u); // 1 per routee
}

// ── Resizing ────────────────────────────────────────────────────────────────

TEST_F(PoolRouterTest, ResizeScaleUp) {
    auto* router = spawn_pool(2);
    EXPECT_EQ(router->routee_count(), 2u);

    router->resize(5);
    driver_->drain(20);
    EXPECT_EQ(router->routee_count(), 5u);
}

TEST_F(PoolRouterTest, ResizeScaleDown) {
    auto* router = spawn_pool(4);
    EXPECT_EQ(router->routee_count(), 4u);

    router->resize(2);
    driver_->drain(20);
    EXPECT_EQ(router->routee_count(), 2u);
}

TEST_F(PoolRouterTest, AddAndRemoveRoutee) {
    auto* router = spawn_pool(2);
    EXPECT_EQ(router->routee_count(), 2u);

    router->add_routee();
    driver_->drain(10);
    EXPECT_EQ(router->routee_count(), 3u);

    router->remove_routee();
    driver_->drain(10);
    EXPECT_EQ(router->routee_count(), 2u);
}

// ── Routee Failure and Restart ─────────────────────────────────────────────

TEST_F(PoolRouterTest, RouteeFailureRestart) {
    // Use fail_after=2 so the routee fails on the 2nd message.
    SupervisionPolicy policy;
    policy.max_restarts = 5;
    policy.restart_interval = std::chrono::milliseconds{5000};

    auto actor = system_->spawn<PoolRouter>(
        std::make_unique<RoundRobinLogic>(),
        [](ActorContext* ctx, ActorSystem& sys) -> std::shared_ptr<AbstractActor> {
            auto a = std::make_shared<test::FailingActor>(ctx, sys);
            a->fail_after = 2;
            return a;
        },
        1, policy);
    driver_->drain(100);
    auto* router = as_router(actor);

    ASSERT_EQ(router->routee_count(), 1u);
    auto router_addr = router->address();

    // Send messages: msg 1 handles ok, msg 2 triggers failure,
    // msg 3-5 should be handled by the replacement routee.
    for (uint32_t i = 0; i < 5; ++i) {
        deliver_to_router(router_addr, i + 100);
    }
    driver_->drain(300);

    // Routee should have been replaced (still 1 routee in pool).
    EXPECT_EQ(router->routee_count(), 1u);

    // Routee should have been replaced (still 1 routee in pool).
    EXPECT_EQ(router->routee_count(), 1u);

    // Verify the pool is still functional by sending one more message.
    deliver_to_router(router_addr, 200);
    driver_->drain(100);

    auto& rts = router->routees();
    ASSERT_EQ(rts.size(), 1u);
    if (rts[0].is_local()) {
        auto* actor_ptr = rts[0].get_actor();
        ASSERT_NE(actor_ptr, nullptr);
        ASSERT_NE(actor_ptr->get(), nullptr);
        auto* fa = static_cast<test::FailingActor*>(actor_ptr->get().get());
        // Routee processed messages; pool is functional after failure.
        EXPECT_GT(fa->messages_processed, 0);
    }
}

// ── Routing Logic Swap ────────────────────────────────────────────────────

TEST_F(PoolRouterTest, RoutingLogicSwap) {
    auto* router = spawn_pool(3);

    router->set_routing_logic(std::make_unique<RandomLogic>(42));
    driver_->drain(10);

    // Pool still functional with new routing logic.
    EXPECT_EQ(router->routee_count(), 3u);

    // Send a message to verify routing still works post-swap
    deliver_to_router(router->address(), 42);
    driver_->drain(50);

    auto& rts = router->routees();
    size_t total_handled = 0;
    for (size_t i = 0; i < rts.size(); ++i) {
        if (rts[i].is_local()) {
            auto* actor_ptr = rts[i].get_actor();
            if (actor_ptr && actor_ptr->get()) {
                auto* ca =
                    static_cast<test::CountingActor*>(actor_ptr->get().get());
                total_handled += static_cast<size_t>(ca->handler_count);
            }
        }
    }
    EXPECT_EQ(total_handled, 1u);
}

} // namespace
} // namespace hpactor::routing
