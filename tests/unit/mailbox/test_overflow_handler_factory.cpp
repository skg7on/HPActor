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

#include <hpactor/mailbox/detail/overflow_handler_factory.hpp>

#include <gtest/gtest.h>

using namespace hpactor::mailbox;
using namespace hpactor::mailbox::detail;

struct TestMsg {
    int x;
};

TEST(OverflowHandlerFactoryTest, MapsEachEnumToCorrectPolicy) {
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::RejectNewest)->policy(),
              OverflowPolicy::RejectNewest);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::DropNewest)->policy(),
              OverflowPolicy::DropNewest);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::DropOldest)->policy(),
              OverflowPolicy::DropOldest);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::DeadLetter)->policy(),
              OverflowPolicy::DeadLetter);
    EXPECT_EQ(make_overflow_handler<TestMsg>(OverflowPolicy::SignalOnly)->policy(),
              OverflowPolicy::SignalOnly);
    EXPECT_EQ(
        make_overflow_handler<TestMsg>(OverflowPolicy::SpillToOverflowQueue)->policy(),
        OverflowPolicy::SpillToOverflowQueue);
}

TEST(OverflowHandlerFactoryTest, DropLowestPriorityPolicyHasDedicatedHandler) {
    EXPECT_EQ(
        make_overflow_handler<TestMsg>(OverflowPolicy::DropLowestPriority)->policy(),
        OverflowPolicy::DropLowestPriority);
}

TEST(OverflowHandlerFactoryTest, UnimplementedPoliciesFallBackToRejectNewest) {
    EXPECT_EQ(
        make_overflow_handler<TestMsg>(OverflowPolicy::BlockWhenAllowed)->policy(),
        OverflowPolicy::RejectNewest);
}
