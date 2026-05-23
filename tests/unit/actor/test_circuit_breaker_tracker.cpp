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
#include <hpactor/actor/circuit_breaker.hpp>

#include <string>

using namespace hpactor;

TEST(CircuitBreakerTrackerTest, DefaultStateIsClosed) {
    CircuitBreakerTracker t;
    EXPECT_EQ(t.state, CircuitBreakerState::kClosed);
    EXPECT_EQ(t.trip_count, 0u);
    EXPECT_FALSE(t.half_open_probe_in_flight);
}

TEST(CircuitBreakerTrackerTest, StateTransitionClosedToOpen) {
    CircuitBreakerTracker t;
    t.state = CircuitBreakerState::kOpen;
    EXPECT_EQ(t.state, CircuitBreakerState::kOpen);
}

TEST(CircuitBreakerTrackerTest, StateTransitionOpenToHalfOpen) {
    CircuitBreakerTracker t;
    t.state = CircuitBreakerState::kOpen;
    t.state = CircuitBreakerState::kHalfOpen;
    EXPECT_EQ(t.state, CircuitBreakerState::kHalfOpen);
}

TEST(CircuitBreakerTrackerTest, StateTransitionHalfOpenToClosed) {
    CircuitBreakerTracker t;
    t.state = CircuitBreakerState::kHalfOpen;
    t.state = CircuitBreakerState::kClosed;
    EXPECT_EQ(t.state, CircuitBreakerState::kClosed);
    EXPECT_EQ(t.trip_count, 0u);
}

TEST(CircuitBreakerTrackerTest, TripCountIncrements) {
    CircuitBreakerTracker t;
    t.trip_count = 1;
    EXPECT_EQ(t.trip_count, 1u);
    t.trip_count = 3;
    EXPECT_EQ(t.trip_count, 3u);
}

TEST(CircuitBreakerTrackerTest, HalfOpenProbeFlag) {
    CircuitBreakerTracker t;
    t.half_open_probe_in_flight = true;
    EXPECT_TRUE(t.half_open_probe_in_flight);
    t.half_open_probe_in_flight = false;
    EXPECT_FALSE(t.half_open_probe_in_flight);
}

TEST(CircuitBreakerTrackerTest, ToStringClosed) {
    EXPECT_EQ(std::string(to_string(CircuitBreakerState::kClosed)), "closed");
}

TEST(CircuitBreakerTrackerTest, ToStringOpen) {
    EXPECT_EQ(std::string(to_string(CircuitBreakerState::kOpen)), "open");
}

TEST(CircuitBreakerTrackerTest, ToStringHalfOpen) {
    EXPECT_EQ(std::string(to_string(CircuitBreakerState::kHalfOpen)), "half_"
                                                                      "open");
}

TEST(CircuitBreakerTrackerTest, EmaDefaults) {
    CircuitBreakerTracker t;
    EXPECT_DOUBLE_EQ(t.failure_ema, 0.0);
}
