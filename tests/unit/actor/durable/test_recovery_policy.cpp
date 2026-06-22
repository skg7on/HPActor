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

#include <hpactor/actor/durable/recovery_policy.hpp>

namespace hpactor::actor::durable {

TEST(RecoveryPolicyTest, ValuesAreDistinct) {
    EXPECT_NE(static_cast<uint8_t>(RecoveryPolicy::FailActor),
              static_cast<uint8_t>(RecoveryPolicy::QuarantineActor));
    EXPECT_NE(static_cast<uint8_t>(RecoveryPolicy::FailActor),
              static_cast<uint8_t>(RecoveryPolicy::SkipCorruptEvent));
    EXPECT_NE(static_cast<uint8_t>(RecoveryPolicy::QuarantineActor),
              static_cast<uint8_t>(RecoveryPolicy::SkipCorruptEvent));
}

TEST(RecoveryPolicyTest, ToStringIsSnakeCase) {
    EXPECT_STREQ(to_string(RecoveryPolicy::FailActor), "fail_actor");
    EXPECT_STREQ(to_string(RecoveryPolicy::QuarantineActor), "quarantine_actor");
    EXPECT_STREQ(to_string(RecoveryPolicy::SkipCorruptEvent), "skip_corrupt_event");
}

} // namespace hpactor::actor::durable
