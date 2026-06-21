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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/receptionist/service_key.hpp>
#include <hpactor/actor/routing/group_router.hpp>
#include <hpactor/actor/routing/routing_logic.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <memory>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

namespace hpactor::routing {
namespace {

static GroupRouter* as_router(Actor& actor) {
    return static_cast<GroupRouter*>(actor.get().get());
}

class GroupRouterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg = test::config_with_scheduler(1);
        system_ = std::make_unique<ActorSystem>(cfg);
        driver_ = std::make_unique<test::SchedulerTestDriver>(*system_);
    }

    GroupRouter* spawn_router(const std::string& key = "test-service") {
        auto actor =
            system_->spawn<GroupRouter>(std::make_unique<RoundRobinLogic>(), key);
        driver_->drain(100);
        return as_router(actor);
    }

    Actor spawn_routee() {
        auto routee = system_->spawn<test::CountingActor>();
        driver_->drain(10);
        return routee;
    }

    void deliver_to_router(const ActorAddress& addr, uint32_t type_id) {
        TypedMessage msg(TypeTag(type_id), StreamBuffer{});
        system_->try_deliver_local(addr.id, std::move(msg));
    }

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<test::SchedulerTestDriver> driver_;
};

// ── Basic Construction ─────────────────────────────────────────────────────

TEST_F(GroupRouterTest, EmptyRoutees) {
    auto* router = spawn_router();
    EXPECT_EQ(router->routee_count(), 0u);
    EXPECT_EQ(router->service_key(), "test-service");

    // Sending to empty group should not crash.
    deliver_to_router(router->address(), 42);
    driver_->drain(10);
    // No-op when no routees available.
}

// ── Routee Management ──────────────────────────────────────────────────────

TEST_F(GroupRouterTest, AddRoutee) {
    auto* router = spawn_router();
    auto routee = spawn_routee();

    router->add_routee(ActorRef(Actor(routee)));
    driver_->drain(10);
    EXPECT_EQ(router->routee_count(), 1u);

    // Send a message — it should be routed to the single routee.
    deliver_to_router(router->address(), 42);
    driver_->drain(50);

    auto* ca = static_cast<test::CountingActor*>(routee.get().get());
    EXPECT_EQ(ca->handler_count, 1);
}

TEST_F(GroupRouterTest, RemoveRoutee) {
    auto* router = spawn_router();
    auto routee_a = spawn_routee();
    auto routee_b = spawn_routee();

    router->add_routee(ActorRef(Actor(routee_a)));
    router->add_routee(ActorRef(Actor(routee_b)));
    driver_->drain(10);
    EXPECT_EQ(router->routee_count(), 2u);

    router->remove_routee(routee_a.address());
    driver_->drain(10);
    EXPECT_EQ(router->routee_count(), 1u);
}

TEST_F(GroupRouterTest, SetRoutees) {
    auto* router = spawn_router();
    auto routee_a = spawn_routee();
    auto routee_b = spawn_routee();

    std::vector<ActorRef> new_routees;
    new_routees.emplace_back(Actor(routee_a));
    new_routees.emplace_back(Actor(routee_b));
    router->set_routees(std::move(new_routees));
    driver_->drain(10);
    EXPECT_EQ(router->routee_count(), 2u);
}

// ── Broadcast ───────────────────────────────────────────────────────────────

TEST_F(GroupRouterTest, Broadcast) {
    auto* router = spawn_router();
    auto routee_a = spawn_routee();
    auto routee_b = spawn_routee();

    router->add_routee(ActorRef(Actor(routee_a)));
    router->add_routee(ActorRef(Actor(routee_b)));
    driver_->drain(10);

    TypedMessage msg(TypeTag(42), StreamBuffer{});
    router->broadcast(std::move(msg));
    driver_->drain(50);

    auto* ca_a = static_cast<test::CountingActor*>(routee_a.get().get());
    auto* ca_b = static_cast<test::CountingActor*>(routee_b.get().get());
    EXPECT_EQ(ca_a->handler_count, 1);
    EXPECT_EQ(ca_b->handler_count, 1);
}

// ── Service Key ────────────────────────────────────────────────────────────

TEST_F(GroupRouterTest, ServiceKey) {
    auto* router = spawn_router("image-processor");
    EXPECT_EQ(router->service_key(), "image-processor");
}

TEST_F(GroupRouterTest, ConstructWithReceptionistServiceKey) {
    receptionist::ServiceKey key{"workers"};
    auto actor =
        system_->spawn<GroupRouter>(key, std::make_unique<RoundRobinLogic>());
    driver_->drain(10);
    auto* router = as_router(actor);
    EXPECT_EQ(router->routee_count(), 0u);
    EXPECT_EQ(router->service_key(), "workers");
}

TEST_F(GroupRouterTest, ServiceKeyRouterStartsWithEmptyRoutees) {
    receptionist::ServiceKey key{"processors"};
    auto actor =
        system_->spawn<GroupRouter>(key, std::make_unique<SmallestMailboxLogic>());
    driver_->drain(10);
    auto* router = as_router(actor);
    // Starts empty; routees populated via Receptionist subscription.
    EXPECT_EQ(router->routee_count(), 0u);
}

// ── Routing Logic Swap ────────────────────────────────────────────────────

TEST_F(GroupRouterTest, RoutingLogicSwap) {
    auto* router = spawn_router();
    auto routee = spawn_routee();
    router->add_routee(ActorRef(Actor(routee)));
    driver_->drain(10);

    router->set_routing_logic(std::make_unique<RandomLogic>(42));
    driver_->drain(10);

    deliver_to_router(router->address(), 42);
    driver_->drain(50);

    auto* ca = static_cast<test::CountingActor*>(routee.get().get());
    EXPECT_EQ(ca->handler_count, 1);
}

} // namespace
} // namespace hpactor::routing
