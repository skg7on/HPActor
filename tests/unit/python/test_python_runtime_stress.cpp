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

#include <hpactor/python/python_runtime.hpp>

#include <array>
#include <atomic>
#include <thread>

using namespace hpactor;

namespace {
struct StressWakeProbe {
    static bool wake(void*) noexcept {
        return true;
    }
};
} // namespace

TEST(PythonRuntimeStressTest, AccountsForEveryConcurrentDispatchAttempt) {
    constexpr size_t kProducers = 4;
    constexpr size_t kAttemptsPerProducer = 2'000;
    python::PythonRuntimeConfig cfg;
    cfg.dispatch_queue_capacity = 256;
    auto created = python::PythonRuntime::create(cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    StressWakeProbe wake;
    ASSERT_TRUE(runtime->start({&wake, &StressWakeProbe::wake}).ok());

    std::atomic<size_t> producers_done{0};
    std::atomic<size_t> accepted{0};
    std::atomic<size_t> rejected{0};
    std::atomic<size_t> consumed{0};
    std::atomic<size_t> max_observed_depth{0};

    std::thread consumer([&] {
        while (producers_done.load(std::memory_order_acquire) != kProducers ||
               runtime->snapshot().queues.dispatch_depth != 0) {
            consumed.fetch_add(runtime->drain_dispatch(64, [](const auto&) {}),
                               std::memory_order_relaxed);
            const size_t depth = runtime->snapshot().queues.dispatch_depth;
            size_t prior = max_observed_depth.load(std::memory_order_relaxed);
            while (prior < depth && !max_observed_depth.compare_exchange_weak(
                                        prior, depth, std::memory_order_relaxed)) {
            }
            std::this_thread::yield();
        }
    });

    std::array<std::thread, kProducers> producers;
    for (size_t producer = 0; producer < kProducers; ++producer) {
        producers[producer] = std::thread([&, producer] {
            for (size_t attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
                auto envelope = std::make_shared<python::PythonDispatchEnvelope>();
                envelope->sequence = producer * kAttemptsPerProducer + attempt;
                if (runtime->try_push_dispatch(envelope)) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();

    EXPECT_EQ(accepted.load() + rejected.load(), 8'000u);
    EXPECT_EQ(consumed.load(), accepted.load());
    EXPECT_EQ(runtime->snapshot().queues.dispatch_rejected, rejected.load());
    EXPECT_LE(max_observed_depth.load(), 256u);
    EXPECT_TRUE(runtime->stop().ok());
}
