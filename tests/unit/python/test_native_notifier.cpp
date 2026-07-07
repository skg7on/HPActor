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

#include <hpactor/python/native_notifier.hpp>

#include <memory>

using namespace hpactor;

TEST(NativeNotifierTest, SignalAndDrainAreNonBlocking) {
    auto created = python::NativeNotifier::create();
    ASSERT_TRUE(created.ok());
    auto notifier = std::move(created.value());
    ASSERT_TRUE(notifier->valid());
    ASSERT_GE(notifier->read_fd(), 0);

    EXPECT_TRUE(notifier->signal());
    EXPECT_TRUE(notifier->signal());
    EXPECT_GE(notifier->drain(), 1u);
    EXPECT_EQ(notifier->drain(), 0u);
}

TEST(NativeNotifierTest, MoveAndClosePreserveSingleOwnership) {
    auto created = python::NativeNotifier::create();
    ASSERT_TRUE(created.ok());
    auto first = std::move(created.value());
    const int fd = first->read_fd();
    python::NativeNotifier moved(std::move(*first));
    EXPECT_FALSE(first->valid());
    EXPECT_EQ(moved.read_fd(), fd);
    moved.close();
    moved.close();
    EXPECT_FALSE(moved.valid());
    EXPECT_FALSE(moved.signal());
}
