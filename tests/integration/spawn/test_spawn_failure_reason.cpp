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

#include <hpactor/actor/spawn.hpp>
#include <hpactor/msg/failure_reason.hpp>

// spawn_errors -> FailureReason mapping
TEST(SpawnFailureReasonTest, SpawnErrorsToFailureReason) {
    using namespace hpactor;

    EXPECT_EQ(failure_reason(spawn_errors::success), FailureReason::Unknown);
    EXPECT_EQ(failure_reason(spawn_errors::unknown_type), FailureReason::NoRoute);
    EXPECT_EQ(failure_reason(spawn_errors::deserialization_failed),
              FailureReason::SerializationError);
    EXPECT_EQ(failure_reason(spawn_errors::node_unreachable),
              FailureReason::NodeUnavailable);
    EXPECT_EQ(failure_reason(spawn_errors::timeout), FailureReason::Timeout);
    EXPECT_EQ(failure_reason(spawn_errors::spawn_receiver_not_running),
              FailureReason::ActorNotReady);

    // Unknown spawn code maps to SpawnFailed
    EXPECT_EQ(failure_reason(99), FailureReason::SpawnFailed);
}

// Verify retryable property for spawn-relevant reasons
TEST(SpawnFailureReasonTest, RetryableForSpawnReasons) {
    using namespace hpactor;

    EXPECT_TRUE(retryable(FailureReason::NoRoute));
    EXPECT_TRUE(retryable(FailureReason::NodeUnavailable));
    EXPECT_TRUE(retryable(FailureReason::ActorNotReady));
    EXPECT_TRUE(retryable(FailureReason::Timeout));
    EXPECT_FALSE(retryable(FailureReason::SerializationError));
    EXPECT_FALSE(retryable(FailureReason::SpawnFailed));
}
