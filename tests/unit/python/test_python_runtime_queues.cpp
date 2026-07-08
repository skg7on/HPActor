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

#include <hpactor/python/python_runtime_queues.hpp>

#include <memory>
#include <vector>

using namespace hpactor;

TEST(PythonRuntimeQueuesTest, DispatchDrainHonorsBudgetAndOrder) {
    python::PythonRuntimeConfig cfg;
    cfg.dispatch_queue_capacity = 64;
    python::PythonRuntimeQueues queues(cfg);
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        auto envelope = std::make_shared<python::PythonDispatchEnvelope>();
        envelope->sequence = sequence;
        ASSERT_TRUE(queues.try_push_dispatch(std::move(envelope)));
    }

    std::vector<uint64_t> observed;
    EXPECT_EQ(queues.drain_dispatch(2,
                                    [&](const auto& envelope) {
                                        observed.push_back(envelope.sequence);
                                    }),
              2u);
    EXPECT_EQ(observed, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(queues.snapshot().dispatch_depth, 1u);
}

TEST(PythonRuntimeQueuesTest, FullQueueRetainsProducerOwnership) {
    python::PythonRuntimeConfig cfg;
    cfg.command_queue_capacity = 64;
    python::PythonRuntimeQueues queues(cfg);
    for (uint64_t i = 0; i < 64; ++i) {
        ASSERT_TRUE(
            queues.try_push_command(std::make_shared<python::PythonCommand>()));
    }
    auto rejected = std::make_shared<python::PythonCommand>();
    EXPECT_FALSE(queues.try_push_command(rejected));
    EXPECT_EQ(rejected.use_count(), 1);
    EXPECT_EQ(queues.snapshot().command_rejected, 1u);
}
