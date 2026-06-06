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

#include <hpactor/mailbox/actor_rate_limiter.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace hpactor::mailbox {
namespace {

uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// 1.1 SteadyRateLimitsConsumption
TEST(ActorRateLimiterTest, SteadyRateLimitsConsumption) {
    ActorRateLimiter limiter;
    limiter.configure(100.0, 10); // 100 msg/s, burst 10

    uint64_t start = now_ns();
    uint64_t admitted = 0;
    uint64_t total_attempts = 0;

    // Simulate 2 seconds: try every 5ms
    while (true) {
        uint64_t now = now_ns();
        if (now - start > 2'000'000'000ULL)
            break;
        if (limiter.try_consume(now)) {
            admitted++;
        }
        total_attempts++;
        // Sleep 5ms between attempts
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // At 100 msg/s over 2 seconds, expect ~200 admitted
    // Tolerance: allow up to 250 to account for burst at start
    EXPECT_GT(admitted, 150);
    EXPECT_LT(admitted, 320);
    EXPECT_GT(total_attempts, 200); // enough attempts were made
}

// 1.2 BurstAllowsSpike
TEST(ActorRateLimiterTest, BurstAllowsSpike) {
    ActorRateLimiter limiter;
    limiter.configure(10.0, 20); // 10 msg/s, burst 20

    uint64_t now = now_ns();

    // Rapid-fire 20 calls — all should succeed (burst)
    uint64_t admitted = 0;
    for (int i = 0; i < 20; i++) {
        if (limiter.try_consume(now)) {
            admitted++;
        }
    }
    EXPECT_EQ(admitted, 20);

    // Next call at same timestamp should fail (burst exhausted)
    EXPECT_FALSE(limiter.try_consume(now));
}

// 1.3 IdleRefill
TEST(ActorRateLimiterTest, IdleRefill) {
    ActorRateLimiter limiter;
    limiter.configure(100.0, 10); // 100 msg/s, burst 10

    uint64_t now = now_ns();

    // Exhaust burst
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(limiter.try_consume(now));
    }
    // One more should fail
    EXPECT_FALSE(limiter.try_consume(now));

    // Wait 100ms — should accumulate ~10 tokens
    now += 100'000'000ULL;
    EXPECT_TRUE(limiter.try_consume(now));
    EXPECT_TRUE(limiter.try_consume(now));
}

// 1.4 DisabledWhenRateZero
TEST(ActorRateLimiterTest, DisabledWhenRateZero) {
    ActorRateLimiter limiter;
    limiter.configure(0.0, 0);

    uint64_t now = now_ns();
    for (int i = 0; i < 10000; i++) {
        EXPECT_TRUE(limiter.try_consume(now));
    }
    EXPECT_FALSE(limiter.is_enabled());
}

// 1.5 CurrentTokensReflectsState
TEST(ActorRateLimiterTest, CurrentTokensReflectsState) {
    ActorRateLimiter limiter;
    limiter.configure(10.0, 5); // 10 msg/s, burst 5

    uint64_t now = now_ns();

    // After init with no refill yet, should be 5 tokens
    EXPECT_DOUBLE_EQ(limiter.current_tokens(), 5.0);

    // Consume 3 tokens
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(limiter.try_consume(now));
    }
    EXPECT_NEAR(limiter.current_tokens(), 2.0, 0.01);
}

// 1.6 TimeUntilNextToken
TEST(ActorRateLimiterTest, TimeUntilNextToken) {
    ActorRateLimiter limiter;
    limiter.configure(10.0, 5); // 10 msg/s -> 1 token per 100ms

    uint64_t now = now_ns();

    // Consume all 5 tokens
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(limiter.try_consume(now));
    }

    // Now should need ~100ms for next token
    uint64_t delay = limiter.time_until_next_token_ns(now);
    EXPECT_GT(delay, 50'000'000ULL);  // > 50ms
    EXPECT_LT(delay, 200'000'000ULL); // < 200ms

    // After waiting, token should be available
    uint64_t later = now + delay + 1'000'000ULL; // a bit more than delay
    EXPECT_TRUE(limiter.try_consume(later));
}

// 1.7 TimeUntilNextTokenReturnsMaxWhenDisabled
TEST(ActorRateLimiterTest, TimeUntilNextTokenReturnsMaxWhenDisabled) {
    ActorRateLimiter limiter;
    limiter.configure(0.0, 0);

    uint64_t now = now_ns();
    EXPECT_EQ(limiter.time_until_next_token_ns(now),
              std::numeric_limits<uint64_t>::max());
}

} // namespace
} // namespace hpactor::mailbox
