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
#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/msg/enqueue_result.hpp>

using namespace hpactor;
using namespace hpactor::mailbox;

TEST(DeliveryModeTest, ToStringRoundTrip) {
    EXPECT_STREQ(to_string(DeliveryMode::BestEffort), "best_effort");
    EXPECT_STREQ(to_string(DeliveryMode::ObservableBestEffort), "observable_"
                                                                "best_effort");
    EXPECT_STREQ(to_string(DeliveryMode::AtLeastOnce), "at_least_once");
    EXPECT_STREQ(to_string(DeliveryMode::DurableAtLeastOnce), "durable_at_"
                                                              "least_once");
}

TEST(DeliveryModeTest, Uint8Size) {
    EXPECT_EQ(sizeof(DeliveryMode), 1);
}

TEST(DeliveryModeTest, DefaultDeliveryOptionsBestEffort) {
    DeliveryOptions opts{};
    EXPECT_EQ(opts.delivery_mode, DeliveryMode::BestEffort);
    EXPECT_FALSE(opts.no_drop);
    EXPECT_FALSE(opts.allow_blocking);
    EXPECT_TRUE(opts.emit_backpressure);
    EXPECT_EQ(opts.message_id, 0);
    EXPECT_EQ(opts.flags, 0);
}

TEST(DeliveryModeTest, IsTrackedDelivery) {
    EXPECT_FALSE(is_tracked_delivery(DeliveryMode::BestEffort));
    EXPECT_FALSE(is_tracked_delivery(DeliveryMode::ObservableBestEffort));
    EXPECT_TRUE(is_tracked_delivery(DeliveryMode::AtLeastOnce));
    EXPECT_TRUE(is_tracked_delivery(DeliveryMode::DurableAtLeastOnce));
}

TEST(DeliveryModeTest, ExplicitDeliveryModeInOptions) {
    DeliveryOptions opts;
    opts.delivery_mode = DeliveryMode::AtLeastOnce;
    EXPECT_EQ(opts.delivery_mode, DeliveryMode::AtLeastOnce);
    opts.delivery_mode = DeliveryMode::DurableAtLeastOnce;
    EXPECT_EQ(opts.delivery_mode, DeliveryMode::DurableAtLeastOnce);
}
