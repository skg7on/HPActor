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

#include <hpactor/actor/routing/routing_logic.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <vector>

#include "system_test_fixture.hpp"

namespace hpactor::routing {
namespace {

class RoutingLogicTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg_ = test::config_with_scheduler(1);
        system_ = std::make_unique<ActorSystem>(cfg_);

        // Spawn 3 CountingActor routees for tests
        routee_a_ = system_->spawn<test::CountingActor>();
        routee_b_ = system_->spawn<test::CountingActor>();
        routee_c_ = system_->spawn<test::CountingActor>();

        routees_.push_back(ActorRef(Actor(routee_a_)));
        routees_.push_back(ActorRef(Actor(routee_b_)));
        routees_.push_back(ActorRef(Actor(routee_c_)));

        // Create mock mailbox snapshots with differing depths
        snapshot_a_.depth = 5;
        snapshot_b_.depth = 0; // smallest
        snapshot_c_.depth = 3;
        states_ = {snapshot_a_, snapshot_b_, snapshot_c_};
    }

    Config cfg_;
    std::unique_ptr<ActorSystem> system_;
    Actor routee_a_;
    Actor routee_b_;
    Actor routee_c_;
    std::vector<ActorRef> routees_;
    cli::MboxSnapshot snapshot_a_;
    cli::MboxSnapshot snapshot_b_;
    cli::MboxSnapshot snapshot_c_;
    std::vector<cli::MboxSnapshot> states_;
};

// ── RoundRobinLogic ───────────────────────────────────────────────────────

TEST_F(RoutingLogicTest, RoundRobinLogic_Sequential) {
    RoundRobinLogic logic;
    TypedMessage msg(TypeTag(42), StreamBuffer{});

    // 6 messages → indices cycle [0,1,2,0,1,2]
    EXPECT_EQ(logic.select_routee(routees_, msg, states_), 0u);
    EXPECT_EQ(logic.select_routee(routees_, msg, states_), 1u);
    EXPECT_EQ(logic.select_routee(routees_, msg, states_), 2u);
    EXPECT_EQ(logic.select_routee(routees_, msg, states_), 0u);
    EXPECT_EQ(logic.select_routee(routees_, msg, states_), 1u);
    EXPECT_EQ(logic.select_routee(routees_, msg, states_), 2u);
}

TEST_F(RoutingLogicTest, RoundRobinLogic_SingleRoutee) {
    RoundRobinLogic logic;
    TypedMessage msg(TypeTag(42), StreamBuffer{});
    std::vector<ActorRef> single = {routees_[0]};
    std::vector<cli::MboxSnapshot> single_states = {states_[0]};

    EXPECT_EQ(logic.select_routee(single, msg, single_states), 0u);
    EXPECT_EQ(logic.select_routee(single, msg, single_states), 0u);
    EXPECT_EQ(logic.select_routee(single, msg, single_states), 0u);
}

TEST_F(RoutingLogicTest, RoundRobinLogic_EmptyRoutees) {
    RoundRobinLogic logic;
    TypedMessage msg(TypeTag(42), StreamBuffer{});
    std::vector<ActorRef> empty;
    std::vector<cli::MboxSnapshot> empty_states;

    // Returns 0 safely even with empty list
    EXPECT_EQ(logic.select_routee(empty, msg, empty_states), 0u);
}

TEST_F(RoutingLogicTest, RoundRobinLogic_Name) {
    RoundRobinLogic logic;
    EXPECT_STREQ(logic.name(), "round-robin");
}

// ── RandomLogic ───────────────────────────────────────────────────────────

TEST_F(RoutingLogicTest, RandomLogic_Distribution) {
    RandomLogic logic(42); // seeded
    TypedMessage msg(TypeTag(42), StreamBuffer{});

    size_t counts[3] = {0, 0, 0};
    constexpr size_t kIterations = 3000;
    for (size_t i = 0; i < kIterations; ++i) {
        size_t idx = logic.select_routee(routees_, msg, states_);
        ASSERT_LT(idx, routees_.size());
        ++counts[idx];
    }
    // Each routee gets roughly 1/3 of the messages (allow 25-42% range for
    // statistical variance at 3k)
    for (size_t i = 0; i < 3; ++i) {
        double pct = static_cast<double>(counts[i]) / kIterations;
        EXPECT_GT(pct, 0.25);
        EXPECT_LT(pct, 0.42);
    }
}

TEST_F(RoutingLogicTest, RandomLogic_SeededReproducible) {
    RandomLogic a(12345);
    RandomLogic b(12345);
    TypedMessage msg(TypeTag(42), StreamBuffer{});

    // Same seed → same sequence of 20 values
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(a.select_routee(routees_, msg, states_),
                  b.select_routee(routees_, msg, states_));
    }
}

TEST_F(RoutingLogicTest, RandomLogic_SingleRoutee) {
    RandomLogic logic(42);
    TypedMessage msg(TypeTag(42), StreamBuffer{});
    std::vector<ActorRef> single = {routees_[0]};
    std::vector<cli::MboxSnapshot> single_states = {states_[0]};

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(logic.select_routee(single, msg, single_states), 0u);
    }
}

TEST_F(RoutingLogicTest, RandomLogic_Name) {
    RandomLogic logic;
    EXPECT_STREQ(logic.name(), "random");
}

// ── ConsistentHashingLogic ─────────────────────────────────────────────────

TEST_F(RoutingLogicTest, ConsistentHashingLogic_SameKeySameRoutee) {
    ConsistentHashingLogic logic(128);
    logic.rebuild_ring(routees_);

    // Same TypeTag → same routee every time
    TypedMessage msg_a(TypeTag(100), StreamBuffer{});
    size_t first = logic.select_routee(routees_, msg_a, states_);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(logic.select_routee(routees_, msg_a, states_), first);
    }
}

TEST_F(RoutingLogicTest, ConsistentHashingLogic_DifferentKeysMayDiffer) {
    ConsistentHashingLogic logic(128);
    logic.rebuild_ring(routees_);

    // Two different keys should go to the same or different nodes depending
    // on hash. At minimum, each call returns a valid index.
    TypedMessage msg_a(TypeTag(100), StreamBuffer{});
    TypedMessage msg_b(TypeTag(200), StreamBuffer{});
    size_t idx_a = logic.select_routee(routees_, msg_a, states_);
    size_t idx_b = logic.select_routee(routees_, msg_b, states_);

    EXPECT_LT(idx_a, routees_.size());
    EXPECT_LT(idx_b, routees_.size());
    // They MAY differ (probabilistically likely). We don't assert inequality
    // since hash collisions are possible.
}

TEST_F(RoutingLogicTest, ConsistentHashingLogic_HashRingRebuildsOnChange) {
    ConsistentHashingLogic logic(128);
    logic.rebuild_ring(routees_);

    TypedMessage msg(TypeTag(42), StreamBuffer{});
    // Verify the ring works with the full set
    (void)logic.select_routee(routees_, msg, states_);

    // Remove a routee and rebuild
    auto reduced = std::vector<ActorRef>{routees_[0], routees_[1]};
    logic.rebuild_ring(reduced);

    size_t after = logic.select_routee(reduced, msg, states_);
    EXPECT_LT(after, 2u);
}

TEST_F(RoutingLogicTest, ConsistentHashingLogic_EmptyRoutees) {
    ConsistentHashingLogic logic;
    logic.rebuild_ring({});
    TypedMessage msg(TypeTag(42), StreamBuffer{});
    std::vector<ActorRef> empty;
    std::vector<cli::MboxSnapshot> empty_states;

    EXPECT_EQ(logic.select_routee(empty, msg, empty_states), 0u);
}

TEST_F(RoutingLogicTest, ConsistentHashingLogic_Name) {
    ConsistentHashingLogic logic;
    EXPECT_STREQ(logic.name(), "consistent-hashing");
}

// ── SmallestMailboxLogic ───────────────────────────────────────────────────

TEST_F(RoutingLogicTest, SmallestMailboxLogic_LowestDepthWins) {
    SmallestMailboxLogic logic;
    TypedMessage msg(TypeTag(42), StreamBuffer{});

    // snapshot_b has depth=0 (lowest) → selects index 1
    size_t idx = logic.select_routee(routees_, msg, states_);
    EXPECT_EQ(idx, 1u);
}

TEST_F(RoutingLogicTest, SmallestMailboxLogic_AllEqual) {
    SmallestMailboxLogic logic;
    TypedMessage msg(TypeTag(42), StreamBuffer{});

    std::vector<cli::MboxSnapshot> equal_states = {
        snapshot_a_, // depth=5
        snapshot_a_, // depth=5
        snapshot_a_, // depth=5
    };
    // All equal → selects first (index 0)
    EXPECT_EQ(logic.select_routee(routees_, msg, equal_states), 0u);
}

TEST_F(RoutingLogicTest, SmallestMailboxLogic_PartialSnapshots) {
    SmallestMailboxLogic logic;
    TypedMessage msg(TypeTag(42), StreamBuffer{});

    // Fewer states than routees — missing states treated as depth=0
    std::vector<cli::MboxSnapshot> partial = {snapshot_a_}; // only 1 of 3
    // Third routee has no snapshot → treated as depth=0 → selected
    size_t idx = logic.select_routee(routees_, msg, partial);
    EXPECT_EQ(idx, 1u); // index 1 has no snapshot, treated as depth 0
    // Wait, that should be index 1 since partial is only {snapshot_a}
    // routee[0] → snapshot_a (depth=5)
    // routee[1] → no snapshot (depth=0, default)
    // routee[2] → no snapshot (depth=0, default)
    // First routee with depth 0 wins → index 1
}

TEST_F(RoutingLogicTest, SmallestMailboxLogic_EmptyRoutees) {
    SmallestMailboxLogic logic;
    TypedMessage msg(TypeTag(42), StreamBuffer{});
    std::vector<ActorRef> empty;
    std::vector<cli::MboxSnapshot> empty_states;

    EXPECT_EQ(logic.select_routee(empty, msg, empty_states), 0u);
}

TEST_F(RoutingLogicTest, SmallestMailboxLogic_Name) {
    SmallestMailboxLogic logic;
    EXPECT_STREQ(logic.name(), "smallest-mailbox");
}

// ── on_routees_changed ─────────────────────────────────────────────────────

TEST_F(RoutingLogicTest, OnRouteesChanged_DefaultIsNoOp) {
    // All strategies except ConsistentHashingLogic leave this as a no-op.
    // Verify it doesn't crash.
    RoundRobinLogic rr;
    RandomLogic rnd;
    SmallestMailboxLogic sm;

    // These should be no-ops (no crash = pass)
    rr.on_routees_changed(routees_);
    rnd.on_routees_changed(routees_);
    sm.on_routees_changed(routees_);
}

} // namespace
} // namespace hpactor::routing
