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

#include <hpactor/actor/stream/stream_registry.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace hpactor;

TEST(StreamRegistryTest, TakeReturnsAndRemovesBothRoutes) {
    StreamRegistry registry;
    registry.register_sender(17, ActorId{1});
    registry.register_receiver(17, ActorId{2});

    auto routes = registry.take(17);
    ASSERT_TRUE(routes.sender.has_value());
    ASSERT_TRUE(routes.receiver.has_value());
    EXPECT_EQ(routes.sender.value(), ActorId{1});
    EXPECT_EQ(routes.receiver.value(), ActorId{2});
    EXPECT_FALSE(registry.find_sender(17).has_value());
    EXPECT_FALSE(registry.find_receiver(17).has_value());
}

TEST(StreamRegistryTest, ConcurrentUniqueRegistrationAndRemovalIsConsistent) {
    StreamRegistry registry;
    constexpr uint64_t kThreads = 8;
    constexpr uint64_t kPerThread = 256;
    std::vector<std::thread> workers;
    std::atomic<uint32_t> missing_routes{0};

    for (uint64_t thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            for (uint64_t offset = 0; offset < kPerThread; ++offset) {
                uint64_t stream = thread * kPerThread + offset + 1;
                registry.register_sender(stream, ActorId{stream});
                registry.register_receiver(stream, ActorId{stream + 10'000});
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(registry.sender_count(), kThreads * kPerThread);
    EXPECT_EQ(registry.receiver_count(), kThreads * kPerThread);

    workers.clear();
    for (uint64_t thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            for (uint64_t offset = 0; offset < kPerThread; ++offset) {
                uint64_t stream = thread * kPerThread + offset + 1;
                auto routes = registry.take(stream);
                if (!routes.sender.has_value() || !routes.receiver.has_value()) {
                    missing_routes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(missing_routes.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(registry.sender_count(), 0u);
    EXPECT_EQ(registry.receiver_count(), 0u);
}
