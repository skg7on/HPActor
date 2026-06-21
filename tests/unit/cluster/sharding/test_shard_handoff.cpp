// Copyright 2026 HPActor Contributors
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
#include <hpactor/cluster/sharding/shard_handoff.hpp>

namespace hpactor::cluster::sharding {

TEST(ShardHandoffTest, DefaultStateIsOwned) {
    ShardHandoff handoff(42);
    EXPECT_EQ(handoff.shard_id(), 42);
    EXPECT_EQ(handoff.state(), HandoffState::Owned);
}

TEST(ShardHandoffTest, OwnedToDraining) {
    ShardHandoff handoff(1);
    EXPECT_TRUE(handoff.begin_drain());
    EXPECT_EQ(handoff.state(), HandoffState::Draining);
}

TEST(ShardHandoffTest, DrainingToTransferring) {
    ShardHandoff handoff(1);
    handoff.begin_drain();
    EXPECT_TRUE(handoff.complete_drain());
    EXPECT_EQ(handoff.state(), HandoffState::Transferring);
}

TEST(ShardHandoffTest, TransferringToRecovering) {
    ShardHandoff handoff(1);
    handoff.begin_drain();
    handoff.complete_drain();
    EXPECT_TRUE(handoff.begin_recovery("node-7"));
    EXPECT_EQ(handoff.state(), HandoffState::Recovering);
    EXPECT_EQ(handoff.new_owner(), "node-7");
}

TEST(ShardHandoffTest, RecoveringToActive) {
    ShardHandoff handoff(1);
    handoff.begin_drain();
    handoff.complete_drain();
    handoff.begin_recovery("node-7");
    EXPECT_TRUE(handoff.activate());
    EXPECT_EQ(handoff.state(), HandoffState::Active);
}

TEST(ShardHandoffTest, CannotSkipStates) {
    ShardHandoff handoff(1);
    // Cannot go directly from Owned to Transferring
    EXPECT_FALSE(handoff.complete_drain());
    EXPECT_EQ(handoff.state(), HandoffState::Owned);
}

TEST(ShardHandoffTest, AbortFromDraining) {
    ShardHandoff handoff(1);
    handoff.begin_drain();
    EXPECT_TRUE(handoff.abort());
    EXPECT_EQ(handoff.state(), HandoffState::Owned);
}

TEST(ShardHandoffTest, AllStatesHaveUniqueValues) {
    EXPECT_NE(static_cast<uint8_t>(HandoffState::Owned),
              static_cast<uint8_t>(HandoffState::Draining));
    EXPECT_NE(static_cast<uint8_t>(HandoffState::Draining),
              static_cast<uint8_t>(HandoffState::Transferring));
    EXPECT_NE(static_cast<uint8_t>(HandoffState::Transferring),
              static_cast<uint8_t>(HandoffState::Recovering));
    EXPECT_NE(static_cast<uint8_t>(HandoffState::Recovering),
              static_cast<uint8_t>(HandoffState::Active));
}

} // namespace hpactor::cluster::sharding
