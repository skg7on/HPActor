# Bench Saturate Saturation Benchmark App — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build App 17 (`apps/bench_saturate/`) — a saturation benchmark that discovers the maximum sustainable message rate of the HPActor system by exponentially ramping send throughput until bounded receiver mailboxes overflow.

**Architecture:** Four `EventBasedActor` subclasses (coordinator → senders → receivers → collector) following the existing `apps/bench_perf/` pattern. The coordinator drives an exponential ramp state machine, broadcasting rate changes to senders. Receivers use bounded mailboxes with `DropHead` overflow; atomic drop counters trigger ramp refinement. Dual-mode operation: interactive CLI (`/saturate` command tree) and headless (`--headless <preset>` for CI).

**Tech Stack:** C++20, HPActor framework (EventBasedActor, Behavior, ActorContext, TypedMessage, StreamBuffer, BoundedMailbox, CLI CommandRegistration<T>), no exceptions, no RTTI.

---

## File Structure

### New Files (12)

| File | Responsibility |
|------|---------------|
| `apps/bench_saturate/CMakeLists.txt` | Build definition for app 17 |
| `apps/bench_saturate/messages.hpp` | TypeTags (0x00010200–0x000102FF), payload encode/decode helpers, CPU burn helper |
| `apps/bench_saturate/actors/saturate_collector_actor.hpp` | Streaming percentiles, drop-rate curve, alloc stat deltas, throughput rollup |
| `apps/bench_saturate/actors/saturate_receiver_actor.hpp` | Bounded mailbox, DropHead overflow, atomic drop counter, latency extraction |
| `apps/bench_saturate/actors/saturate_sender_actor.hpp` | Self-scheduling tick loop, rate-directed dispatch, round-robin to receivers |
| `apps/bench_saturate/actors/saturate_coordinator_actor.hpp` | Ramp state machine, preset management, rate broadcasting, result aggregation |
| `apps/bench_saturate/commands/saturate_commands.cpp` | CLI commands via `CommandRegistration<T>` |
| `apps/bench_saturate/17_bench_saturate.cpp` | Main: system probe, splash screen, spawn actors, headless mode, event loop |
| `tests/unit/saturate/test_saturate_messages.cpp` | Payload encode/decode roundtrip, edge cases |
| `tests/unit/saturate/test_saturate_collector_math.cpp` | Percentile computation, drop-curve math, throughput calculation |
| `tests/unit/saturate/CMakeLists.txt` | Unit test build definition |

### Modified Files (2)

| File | Change |
|------|--------|
| `apps/CMakeLists.txt` | Add `add_subdirectory(bench_saturate)` |
| (test registration) | Add saturate test subdirectories |

---

## Phase 1: Foundation — Messages & Build System

### Task 1: Create messages.hpp with TypeTags and payload helpers

**Files:**
- Create: `apps/bench_saturate/messages.hpp`

- [ ] **Step 1: Write the file**

```cpp
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

#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstring>
#include <random>

namespace hpactor::apps::bench_saturate {

// =============================================================================
// Message Type Tags — application range (0x00010200 – 0x000102FF)
// =============================================================================

inline constexpr TypeTag SaturateStartTag{0x00010200};
inline constexpr TypeTag SaturateStopTag{0x00010201};
inline constexpr TypeTag RateChangeTag{0x00010202};
inline constexpr TypeTag StatsPollTag{0x00010203};
inline constexpr TypeTag StatsReplyTag{0x00010204};
inline constexpr TypeTag ThroughputSampleTag{0x00010205};
inline constexpr TypeTag DropReportTag{0x00010206};
inline constexpr TypeTag LatencySampleTag{0x00010207};
inline constexpr TypeTag LoadMessageTag{0x00010208};

// =============================================================================
// Payload mode enum
// =============================================================================

enum class PayloadMode : uint8_t {
    Small = 0,  // header-only (20 bytes)
    Junk = 1,   // header + random fill to [min, max] size
    Mixed = 2   // 80% small, 20% junk
};

// =============================================================================
// SaturateStart payload encoding
// =============================================================================

struct SaturateStartPayload {
    uint32_t num_senders = 100;
    uint32_t num_receivers = 10;
    PayloadMode payload_mode = PayloadMode::Small;
    uint16_t payload_size_min = 16;
    uint16_t payload_size_max = 16;
    uint32_t initial_rate_msgps = 100;
    uint16_t step_interval_ms = 1000;
    float drop_threshold_pct = 1.0f;
    uint8_t refine_iterations = 5;
    uint32_t mailbox_capacity = 4096;
    uint32_t stable_duration_ms = 5000;
    uint32_t duration_max_ms = 120000;

    StreamBuffer encode() const {
        uint8_t buf[40];
        std::memset(buf, 0, sizeof(buf));
        size_t off = 0;
        std::memcpy(buf + off, &num_senders, 4); off += 4;
        std::memcpy(buf + off, &num_receivers, 4); off += 4;
        uint8_t mode = static_cast<uint8_t>(payload_mode);
        std::memcpy(buf + off, &mode, 1); off += 1;
        std::memcpy(buf + off, &payload_size_min, 2); off += 2;
        std::memcpy(buf + off, &payload_size_max, 2); off += 2;
        std::memcpy(buf + off, &initial_rate_msgps, 4); off += 4;
        std::memcpy(buf + off, &step_interval_ms, 2); off += 2;
        std::memcpy(buf + off, &drop_threshold_pct, 4); off += 4;
        std::memcpy(buf + off, &refine_iterations, 1); off += 1;
        std::memcpy(buf + off, &mailbox_capacity, 4); off += 4;
        std::memcpy(buf + off, &stable_duration_ms, 4); off += 4;
        std::memcpy(buf + off, &duration_max_ms, 4); off += 4;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static SaturateStartPayload decode(const StreamBuffer& buf) {
        SaturateStartPayload p;
        if (buf.size() < 40) return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.num_senders, d + off, 4); off += 4;
        std::memcpy(&p.num_receivers, d + off, 4); off += 4;
        uint8_t mode = 0;
        std::memcpy(&mode, d + off, 1); off += 1;
        p.payload_mode = static_cast<PayloadMode>(mode);
        std::memcpy(&p.payload_size_min, d + off, 2); off += 2;
        std::memcpy(&p.payload_size_max, d + off, 2); off += 2;
        std::memcpy(&p.initial_rate_msgps, d + off, 4); off += 4;
        std::memcpy(&p.step_interval_ms, d + off, 2); off += 2;
        std::memcpy(&p.drop_threshold_pct, d + off, 4); off += 4;
        std::memcpy(&p.refine_iterations, d + off, 1); off += 1;
        std::memcpy(&p.mailbox_capacity, d + off, 4); off += 4;
        std::memcpy(&p.stable_duration_ms, d + off, 4); off += 4;
        std::memcpy(&p.duration_max_ms, d + off, 4); off += 4;
        return p;
    }
};

// =============================================================================
// RateChange payload encoding
// =============================================================================

struct RateChangePayload {
    uint32_t target_rate_msgps = 100;
    PayloadMode payload_mode = PayloadMode::Small;
    uint16_t payload_size_min = 16;
    uint16_t payload_size_max = 16;
    uint16_t step_interval_ms = 1000;

    StreamBuffer encode() const {
        uint8_t buf[12];
        std::memset(buf, 0, sizeof(buf));
        size_t off = 0;
        std::memcpy(buf + off, &target_rate_msgps, 4); off += 4;
        uint8_t mode = static_cast<uint8_t>(payload_mode);
        std::memcpy(buf + off, &mode, 1); off += 1;
        std::memcpy(buf + off, &payload_size_min, 2); off += 2;
        std::memcpy(buf + off, &payload_size_max, 2); off += 2;
        std::memcpy(buf + off, &step_interval_ms, 2); off += 2;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static RateChangePayload decode(const StreamBuffer& buf) {
        RateChangePayload p;
        if (buf.size() < 12) return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.target_rate_msgps, d + off, 4); off += 4;
        uint8_t mode = 0;
        std::memcpy(&mode, d + off, 1); off += 1;
        p.payload_mode = static_cast<PayloadMode>(mode);
        std::memcpy(&p.payload_size_min, d + off, 2); off += 2;
        std::memcpy(&p.payload_size_max, d + off, 2); off += 2;
        std::memcpy(&p.step_interval_ms, d + off, 2); off += 2;
        return p;
    }
};

// =============================================================================
// LoadMessage payload encoding (sent from sender to receiver)
// =============================================================================

struct LoadMessagePayload {
    static constexpr size_t kHeaderSize = 20; // sender_id(4) + seq_no(8) + timestamp(8)

    uint32_t sender_id = 0;
    uint64_t seq_no = 0;
    uint64_t send_timestamp_us = 0;

    StreamBuffer encode_header() const {
        uint8_t buf[kHeaderSize];
        size_t off = 0;
        std::memcpy(buf + off, &sender_id, 4); off += 4;
        std::memcpy(buf + off, &seq_no, 8); off += 8;
        std::memcpy(buf + off, &send_timestamp_us, 8); off += 8;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    StreamBuffer encode_with_junk(size_t total_size, uint64_t random_seed) const {
        if (total_size < kHeaderSize) total_size = kHeaderSize;
        StreamBuffer buf(total_size);
        uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(d + off, &sender_id, 4); off += 4;
        std::memcpy(d + off, &seq_no, 8); off += 8;
        std::memcpy(d + off, &send_timestamp_us, 8); off += 8;
        uint64_t seed = random_seed;
        for (size_t i = kHeaderSize; i < total_size; ++i) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            d[i] = static_cast<uint8_t>(seed >> 32);
        }
        return buf;
    }

    static LoadMessagePayload decode(const StreamBuffer& buf) {
        LoadMessagePayload p;
        if (buf.size() < kHeaderSize) return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.sender_id, d + off, 4); off += 4;
        std::memcpy(&p.seq_no, d + off, 8); off += 8;
        std::memcpy(&p.send_timestamp_us, d + off, 8); off += 8;
        return p;
    }
};

// =============================================================================
// LatencySample payload encoding
// =============================================================================

struct LatencySamplePayload {
    uint32_t sender_id = 0;
    uint64_t seq_no = 0;
    uint32_t latency_us = 0;

    StreamBuffer encode() const {
        uint8_t buf[16];
        size_t off = 0;
        std::memcpy(buf + off, &sender_id, 4); off += 4;
        std::memcpy(buf + off, &seq_no, 8); off += 8;
        std::memcpy(buf + off, &latency_us, 4); off += 4;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static LatencySamplePayload decode(const StreamBuffer& buf) {
        LatencySamplePayload p;
        if (buf.size() < 16) return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.sender_id, d + off, 4); off += 4;
        std::memcpy(&p.seq_no, d + off, 8); off += 8;
        std::memcpy(&p.latency_us, d + off, 4); off += 4;
        return p;
    }
};

// =============================================================================
// DropReport payload encoding
// =============================================================================

struct DropReportPayload {
    uint32_t receiver_id = 0;
    uint64_t total_received = 0;
    uint32_t total_dropped = 0;

    StreamBuffer encode() const {
        uint8_t buf[16];
        size_t off = 0;
        std::memcpy(buf + off, &receiver_id, 4); off += 4;
        std::memcpy(buf + off, &total_received, 8); off += 8;
        std::memcpy(buf + off, &total_dropped, 4); off += 4;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static DropReportPayload decode(const StreamBuffer& buf) {
        DropReportPayload p;
        if (buf.size() < 16) return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.receiver_id, d + off, 4); off += 4;
        std::memcpy(&p.total_received, d + off, 8); off += 8;
        std::memcpy(&p.total_dropped, d + off, 4); off += 4;
        return p;
    }
};

// =============================================================================
// Convenience: wrap a StreamBuffer in a TypedMessage
// =============================================================================

inline TypedMessage make_msg(TypeTag tag, StreamBuffer payload = {}) {
    return TypedMessage(tag, std::move(payload));
}

// =============================================================================
// CPU Burn helper (portable, cooperative)
// =============================================================================

inline void burn_cpu_us(uint64_t us) {
    if (us == 0) return;
    auto target = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < target) {
        // spin
    }
}

// =============================================================================
// Payload size generator for junk/mixed modes
// =============================================================================

inline size_t random_payload_size(uint16_t min_size, uint16_t max_size, uint64_t& seed) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    uint16_t range = max_size - min_size + 1;
    return min_size + static_cast<size_t>((seed >> 32) % range);
}

} // namespace hpactor::apps::bench_saturate
```

- [ ] **Step 2: Verify it compiles**

```bash
ninja -C build 17_bench_saturate 2>&1 | head -20
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_saturate/messages.hpp
git commit -m "feat(bench_saturate): add messages.hpp with TypeTags and payload helpers"
```

---

### Task 2: Create CMakeLists.txt for the app

**Files:**
- Create: `apps/bench_saturate/CMakeLists.txt`
- Modify: `apps/CMakeLists.txt`

- [ ] **Step 1: Write app CMakeLists.txt**

```cmake
add_executable(17_bench_saturate
    17_bench_saturate.cpp
    commands/saturate_commands.cpp
)
target_link_libraries(17_bench_saturate PRIVATE hpactor_lib)
target_include_directories(17_bench_saturate PRIVATE ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 2: Register in apps/CMakeLists.txt**

Append after the `bench_perf` line:
```cmake
add_subdirectory(bench_saturate)
```

- [ ] **Step 3: Verify CMake configuration**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF 2>&1 | tail -5
```

- [ ] **Step 4: Commit**

```bash
git add apps/bench_saturate/CMakeLists.txt apps/CMakeLists.txt
git commit -m "feat(bench_saturate): add CMakeLists.txt and register app 17 in build"
```

---

## Phase 2: Unit Tests

### Task 3: Create unit test CMakeLists and message roundtrip tests

**Files:**
- Create: `tests/unit/saturate/CMakeLists.txt`
- Create: `tests/unit/saturate/test_saturate_messages.cpp`

- [ ] **Step 1: Write unit test CMakeLists.txt**

```cmake
add_executable(test_saturate_messages
    test_saturate_messages.cpp
)
target_link_libraries(test_saturate_messages PRIVATE hpactor_lib GTest::gtest GTest::gtest_main)
target_include_directories(test_saturate_messages PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/bench_saturate
)
gtest_discover_tests(test_saturate_messages
    PROPERTIES
        LABELS "unit;saturate"
        TIMEOUT 10
)
```

- [ ] **Step 2: Write failing tests (RED)**

```cpp
#include <gtest/gtest.h>
#include "messages.hpp"

namespace hpactor::apps::bench_saturate {
namespace {

TEST(SaturateMessagesTest, StartPayloadRoundtrip) {
    SaturateStartPayload orig;
    orig.num_senders = 500;
    orig.num_receivers = 50;
    orig.payload_mode = PayloadMode::Junk;
    orig.payload_size_min = 1024;
    orig.payload_size_max = 16384;
    orig.initial_rate_msgps = 1000;
    orig.step_interval_ms = 500;
    orig.drop_threshold_pct = 2.5f;
    orig.refine_iterations = 7;
    orig.mailbox_capacity = 2048;
    orig.stable_duration_ms = 10000;
    orig.duration_max_ms = 60000;

    auto buf = orig.encode();
    auto decoded = SaturateStartPayload::decode(buf);

    EXPECT_EQ(decoded.num_senders, 500);
    EXPECT_EQ(decoded.num_receivers, 50);
    EXPECT_EQ(decoded.payload_mode, PayloadMode::Junk);
    EXPECT_EQ(decoded.payload_size_min, 1024);
    EXPECT_EQ(decoded.payload_size_max, 16384);
    EXPECT_EQ(decoded.initial_rate_msgps, 1000);
    EXPECT_EQ(decoded.step_interval_ms, 500);
    EXPECT_FLOAT_EQ(decoded.drop_threshold_pct, 2.5f);
    EXPECT_EQ(decoded.refine_iterations, 7);
    EXPECT_EQ(decoded.mailbox_capacity, 2048);
    EXPECT_EQ(decoded.stable_duration_ms, 10000);
    EXPECT_EQ(decoded.duration_max_ms, 60000);
}

TEST(SaturateMessagesTest, StartPayloadDecodeTruncated) {
    StreamBuffer small_buf(10);
    auto decoded = SaturateStartPayload::decode(small_buf);
    EXPECT_EQ(decoded.num_senders, 0);
}

TEST(SaturateMessagesTest, RateChangePayloadRoundtrip) {
    RateChangePayload orig;
    orig.target_rate_msgps = 5000;
    orig.payload_mode = PayloadMode::Mixed;
    orig.payload_size_min = 32;
    orig.payload_size_max = 4096;
    orig.step_interval_ms = 2000;

    auto buf = orig.encode();
    auto decoded = RateChangePayload::decode(buf);

    EXPECT_EQ(decoded.target_rate_msgps, 5000);
    EXPECT_EQ(decoded.payload_mode, PayloadMode::Mixed);
    EXPECT_EQ(decoded.payload_size_min, 32);
    EXPECT_EQ(decoded.payload_size_max, 4096);
    EXPECT_EQ(decoded.step_interval_ms, 2000);
}

TEST(SaturateMessagesTest, LoadMessageHeaderRoundtrip) {
    LoadMessagePayload orig;
    orig.sender_id = 42;
    orig.seq_no = 123456789012345ULL;
    orig.send_timestamp_us = 9876543210ULL;

    auto buf = orig.encode_header();
    EXPECT_EQ(buf.size(), LoadMessagePayload::kHeaderSize);
    EXPECT_EQ(buf.size(), 20u);

    auto decoded = LoadMessagePayload::decode(buf);
    EXPECT_EQ(decoded.sender_id, 42);
    EXPECT_EQ(decoded.seq_no, 123456789012345ULL);
    EXPECT_EQ(decoded.send_timestamp_us, 9876543210ULL);
}

TEST(SaturateMessagesTest, LoadMessageJunkSize) {
    LoadMessagePayload orig;
    orig.sender_id = 1; orig.seq_no = 100; orig.send_timestamp_us = 1000;
    size_t total_size = LoadMessagePayload::kHeaderSize + 512;
    auto buf = orig.encode_with_junk(total_size, 42);
    EXPECT_EQ(buf.size(), total_size);
    auto decoded = LoadMessagePayload::decode(buf);
    EXPECT_EQ(decoded.sender_id, 1);
}

TEST(SaturateMessagesTest, LoadMessageJunkDeterministic) {
    LoadMessagePayload orig;
    orig.sender_id = 1; orig.seq_no = 1; orig.send_timestamp_us = 1;
    auto buf1 = orig.encode_with_junk(100, 42);
    auto buf2 = orig.encode_with_junk(100, 42);
    EXPECT_EQ(buf1.size(), buf2.size());
    EXPECT_EQ(std::memcmp(buf1.data(), buf2.data(), buf1.size()), 0);
}

TEST(SaturateMessagesTest, LatencySamplePayloadRoundtrip) {
    LatencySamplePayload orig;
    orig.sender_id = 7; orig.seq_no = 999; orig.latency_us = 1500;
    auto buf = orig.encode();
    auto decoded = LatencySamplePayload::decode(buf);
    EXPECT_EQ(decoded.sender_id, 7);
    EXPECT_EQ(decoded.seq_no, 999);
    EXPECT_EQ(decoded.latency_us, 1500);
}

TEST(SaturateMessagesTest, DropReportPayloadRoundtrip) {
    DropReportPayload orig;
    orig.receiver_id = 3; orig.total_received = 1000000; orig.total_dropped = 500;
    auto buf = orig.encode();
    auto decoded = DropReportPayload::decode(buf);
    EXPECT_EQ(decoded.receiver_id, 3);
    EXPECT_EQ(decoded.total_received, 1000000);
    EXPECT_EQ(decoded.total_dropped, 500);
}

TEST(SaturateMessagesTest, PayloadModeValues) {
    EXPECT_EQ(static_cast<uint8_t>(PayloadMode::Small), 0);
    EXPECT_EQ(static_cast<uint8_t>(PayloadMode::Junk), 1);
    EXPECT_EQ(static_cast<uint8_t>(PayloadMode::Mixed), 2);
}

TEST(SaturateMessagesTest, RandomPayloadSizeInRange) {
    uint64_t seed = 12345;
    for (int i = 0; i < 1000; ++i) {
        size_t sz = random_payload_size(1024, 4096, seed);
        EXPECT_GE(sz, 1024u);
        EXPECT_LE(sz, 4096u);
    }
}

TEST(SaturateMessagesTest, RandomPayloadSizeDeterministic) {
    uint64_t seed1 = 42, seed2 = 42;
    EXPECT_EQ(random_payload_size(100, 200, seed1),
              random_payload_size(100, 200, seed2));
}

} // namespace
} // namespace hpactor::apps::bench_saturate
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build tests/unit/saturate/test_saturate_messages && ./build/tests/unit/saturate/test_saturate_messages
```

Expected: 11 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/saturate/CMakeLists.txt tests/unit/saturate/test_saturate_messages.cpp
git commit -m "test(bench_saturate): add message payload roundtrip unit tests"
```

---

### Task 4: Create collector math unit tests

**Files:**
- Create: `tests/unit/saturate/test_saturate_collector_math.cpp`

- [ ] **Step 1: Write the test file (RED/GREEN)**

```cpp
#include <gtest/gtest.h>
#include <algorithm>
#include <vector>

namespace hpactor::apps::bench_saturate {
namespace {

struct PercentileResult { double p50 = 0.0; double p99 = 0.0; double p999 = 0.0; };

PercentileResult compute_percentiles(const std::vector<double>& sorted) {
    PercentileResult r;
    if (sorted.empty()) return r;
    r.p50 = sorted[sorted.size() / 2];
    r.p99 = sorted[sorted.size() * 99 / 100];
    r.p999 = sorted[sorted.size() * 999 / 1000];
    return r;
}

double compute_drop_rate_pct(uint64_t dropped, uint64_t total_sent) {
    if (total_sent == 0) return 0.0;
    return 100.0 * static_cast<double>(dropped) / static_cast<double>(total_sent);
}

double compute_throughput_msgps(uint64_t total_msgs, uint64_t elapsed_ms) {
    if (elapsed_ms == 0) return 0.0;
    return static_cast<double>(total_msgs) / (static_cast<double>(elapsed_ms) / 1000.0);
}

TEST(CollectorMathTest, PercentilesOddCount) {
    std::vector<double> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 100};
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 6.0);
    EXPECT_DOUBLE_EQ(r.p99, 100.0);
}

TEST(CollectorMathTest, PercentilesEvenCount) {
    std::vector<double> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 6.0);
    EXPECT_DOUBLE_EQ(r.p99, 10.0);
}

TEST(CollectorMathTest, PercentilesSingleElement) {
    std::vector<double> data = {42.0};
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 42.0);
    EXPECT_DOUBLE_EQ(r.p99, 42.0);
}

TEST(CollectorMathTest, PercentilesEmpty) {
    std::vector<double> data;
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 0.0);
}

TEST(CollectorMathTest, PercentilesAllSameValue) {
    std::vector<double> data(100, 5.0);
    auto r = compute_percentiles(data);
    EXPECT_DOUBLE_EQ(r.p50, 5.0);
    EXPECT_DOUBLE_EQ(r.p99, 5.0);
}

TEST(CollectorMathTest, DropRateZeroDrops) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(0, 1000000), 0.0);
}

TEST(CollectorMathTest, DropRateHalfDropped) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(500, 1000), 50.0);
}

TEST(CollectorMathTest, DropRateAllDropped) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(1000, 1000), 100.0);
}

TEST(CollectorMathTest, DropRateZeroTotal) {
    EXPECT_DOUBLE_EQ(compute_drop_rate_pct(0, 0), 0.0);
}

TEST(CollectorMathTest, ThroughputOneSecond) {
    EXPECT_DOUBLE_EQ(compute_throughput_msgps(1000, 1000), 1000.0);
}

TEST(CollectorMathTest, ThroughputHalfSecond) {
    EXPECT_DOUBLE_EQ(compute_throughput_msgps(500, 500), 1000.0);
}

TEST(CollectorMathTest, ThroughputZeroElapsed) {
    EXPECT_DOUBLE_EQ(compute_throughput_msgps(1000, 0), 0.0);
}

TEST(CollectorMathTest, ReservoirBounded) {
    constexpr size_t kReservoirSize = 100;
    std::vector<double> reservoir;
    reservoir.reserve(kReservoirSize * 2);
    for (size_t i = 0; i < 500; ++i) {
        reservoir.push_back(static_cast<double>(i));
        if (reservoir.size() > kReservoirSize) {
            reservoir.erase(reservoir.begin(),
                           reservoir.begin() + static_cast<ptrdiff_t>(reservoir.size() - kReservoirSize));
        }
    }
    EXPECT_EQ(reservoir.size(), kReservoirSize);
    EXPECT_DOUBLE_EQ(reservoir.front(), 400.0);
    EXPECT_DOUBLE_EQ(reservoir.back(), 499.0);
}

} // namespace
} // namespace hpactor::apps::bench_saturate
```

- [ ] **Step 2: Update CMakeLists.txt**

Append to `tests/unit/saturate/CMakeLists.txt`:

```cmake
add_executable(test_saturate_collector_math
    test_saturate_collector_math.cpp
)
target_link_libraries(test_saturate_collector_math PRIVATE hpactor_lib GTest::gtest GTest::gtest_main)
target_include_directories(test_saturate_collector_math PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(test_saturate_collector_math
    PROPERTIES
        LABELS "unit;saturate"
        TIMEOUT 10
)
```

- [ ] **Step 3: Build and run**

```bash
ninja -C build tests/unit/saturate/test_saturate_collector_math && ./build/tests/unit/saturate/test_saturate_collector_math
```

Expected: 13 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/saturate/test_saturate_collector_math.cpp tests/unit/saturate/CMakeLists.txt
git commit -m "test(bench_saturate): add collector math unit tests"
```

---

## Phase 3: Actors

### Task 5: Implement SaturateCollectorActor

**Files:** Create: `apps/bench_saturate/actors/saturate_collector_actor.hpp`

- [ ] **Step 1: Write the collector (GREEN)**

The collector is an `EventBasedActor` that receives `ThroughputSampleTag`, `LatencySampleTag`, and `DropReportTag` messages. It maintains a 10K-sample latency reservoir, computes streaming percentiles on demand, tracks a drop-rate time-series curve, and calculates throughput. Key methods: `handle_latency_sample()`, `handle_drop_report()`, `recompute_percentiles()`, `serialize_state()` returning key=value report text. Follows `BenchCollectorActor` pattern exactly.

- [ ] **Step 2: Verify compilation**

```bash
ninja -C build 17_bench_saturate 2>&1 | head -10
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_saturate/actors/saturate_collector_actor.hpp
git commit -m "feat(bench_saturate): add SaturateCollectorActor"
```

---

### Task 6: Implement SaturateReceiverActor

**Files:** Create: `apps/bench_saturate/actors/saturate_receiver_actor.hpp`

- [ ] **Step 1: Write the receiver (GREEN)**

The receiver is an `EventBasedActor` with bounded mailbox (`DropHead` overflow). It handles `LoadMessageTag` by extracting latency from payload timestamps, forwarding `LatencySampleTag` to collector, and periodically sending `DropReportTag`. Atomic counters track `received_count_` and `dropped_count_`. Responds to `SaturateStartTag`, `SaturateStopTag`, and `StatsPollTag`.

- [ ] **Step 2: Verify compilation**

```bash
ninja -C build 17_bench_saturate 2>&1 | head -10
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_saturate/actors/saturate_receiver_actor.hpp
git commit -m "feat(bench_saturate): add SaturateReceiverActor"
```

---

### Task 7: Implement SaturateSenderActor

**Files:** Create: `apps/bench_saturate/actors/saturate_sender_actor.hpp`

- [ ] **Step 1: Write the sender (GREEN)**

The sender is an `EventBasedActor` that self-schedules ticks at a coordinator-directed rate. Uses a private `SendTickTag` for scheduling. On each tick, sends a batch of `LoadMessageTag` messages to receivers via round-robin dispatch. Supports three payload modes: small (header-only 20B), junk (random fill to target size), mixed (80/20). Reports throughput to collector periodically via `ThroughputSampleTag`.

- [ ] **Step 2: Verify compilation**

```bash
ninja -C build 17_bench_saturate 2>&1 | head -10
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_saturate/actors/saturate_sender_actor.hpp
git commit -m "feat(bench_saturate): add SaturateSenderActor"
```

---

### Task 8: Implement SaturateCoordinatorActor

**Files:** Create: `apps/bench_saturate/actors/saturate_coordinator_actor.hpp`

- [ ] **Step 1: Write the coordinator (GREEN)**

The coordinator is an `EventBasedActor` with a 5-phase ramp state machine: Idle → Probing (exponential rate doubling, 1s steps) → Refining (binary search, 5 iterations) → Stable (hold at ceiling for 5s observation) → Reporting. Six built-in presets. Handles `SaturateStartTag` (by preset name), broadcasts `RateChangeTag` to senders, polls collector via `StatsPollTag`/`StatsReplyTag`, tracks saturation ceiling. Includes `RampTickTag` for self-scheduling the state machine. Safety cap at `duration_max_ms`.

- [ ] **Step 2: Verify compilation**

```bash
ninja -C build 17_bench_saturate 2>&1 | head -10
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_saturate/actors/saturate_coordinator_actor.hpp
git commit -m "feat(bench_saturate): add SaturateCoordinatorActor"
```

---

## Phase 4: CLI Commands

### Task 9: Implement CLI commands

**Files:** Create: `apps/bench_saturate/commands/saturate_commands.cpp`

- [ ] **Step 1: Write the commands file (GREEN)**

Eight commands registered via `CommandRegistration<T>`:
- `saturate/start` — validates preset, guards against duplicate runs, sends `SaturateStartTag` with preset name payload
- `saturate/stop` — sends `SaturateStopTag` to coordinator
- `saturate/status` — reads coordinator state via `InspectStateRequest`, displays phase/rate/drop%/ceiling
- `saturate/report [detail]` — reads collector state, displays throughput/drops/latency
- `saturate/export [json|csv]` — outputs collector state as JSON object or CSV row
- `saturate/list` — prints the six available presets
- `saturate/probe` — displays system hardware summary
- `saturate/help` — lists all commands

Follows `bench_commands.cpp` pattern exactly: `find_coordinator()` / `find_collector()` helpers via type-name enumeration, `parse_state()` for key=value text parsing.

- [ ] **Step 2: Build**

```bash
ninja -C build 17_bench_saturate 2>&1 | tail -10
```

Expected: links successfully.

- [ ] **Step 3: Commit**

```bash
git add apps/bench_saturate/commands/saturate_commands.cpp
git commit -m "feat(bench_saturate): add CLI commands for /saturate command tree"
```

---

## Phase 5: Main & Headless Mode

### Task 10: Implement main with system probe, splash, and headless mode

**Files:** Create: `apps/bench_saturate/17_bench_saturate.cpp`

- [ ] **Step 1: Write the main file (GREEN)**

Key components:
1. `probe_system()` — detects CPU cores (P/E split on macOS), cache topology, total memory; prints splash screen with preset descriptions
2. `run_headless(preset, format, output_path)` — spawns ActorSystem without CLI, spawns coordinator+collector+senders+receivers, starts the preset run, waits for completion or `duration_max_ms` timeout, outputs JSON/CSV results to stdout or file
3. `main()` — parses `--headless`, `--format`, `--output` args; routes to headless or interactive mode
4. Interactive mode: spawns CLI, spawns max-sized actor pool (5000 senders, 1000 receivers), wires coordinator, blocks on CLI exit, graceful shutdown

- [ ] **Step 2: Build the full app**

```bash
ninja -C build 17_bench_saturate 2>&1 | tail -10
```

Expected: links `17_bench_saturate` executable.

- [ ] **Step 3: Smoke test — headless mode**

```bash
timeout 60 ./build/apps/bench_saturate/17_bench_saturate --headless quick-saturate 2>&1
```

Expected: JSON output with drop rate and throughput within 60s.

- [ ] **Step 4: Commit**

```bash
git add apps/bench_saturate/17_bench_saturate.cpp
git commit -m "feat(bench_saturate): add main with system probe and headless mode"
```

---

## Phase 6: Final Verification

### Task 11: Full build verification and test run

- [ ] **Step 1: Full rebuild**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF 2>&1 | tail -5
ninja -C build 2>&1 | tail -20
```

Expected: full build succeeds without errors.

- [ ] **Step 2: Run unit tests**

```bash
ctest -L "unit" -R "saturate" --output-on-failure 2>&1
```

Expected: all saturate unit tests pass (24 tests).

- [ ] **Step 3: Run headless smoke test**

```bash
./build/apps/bench_saturate/17_bench_saturate --headless quick-saturate --format json 2>&1
```

Expected: valid JSON with `total_sent`, `total_received`, `total_dropped`, `drop_rate_pct`, `throughput_msgps` fields.

- [ ] **Step 4: Verify existing tests unaffected**

```bash
ctest -R "ActorId" --output-on-failure 2>&1
```

Expected: existing tests pass.

- [ ] **Step 5: Final commit**

```bash
git add -A && git diff --cached --stat && git commit -m "chore(bench_saturate): final build verification and fixups"
```

---

## Summary

| Phase | Tasks | Files Created | Tests |
|-------|-------|---------------|-------|
| 1. Foundation | 2 | `messages.hpp`, `CMakeLists.txt` | — |
| 2. Unit Tests | 2 | `test_saturate_messages.cpp`, `test_saturate_collector_math.cpp` | 24 |
| 3. Collector | 1 | `saturate_collector_actor.hpp` | — |
| 4. Receiver | 1 | `saturate_receiver_actor.hpp` | — |
| 5. Sender | 1 | `saturate_sender_actor.hpp` | — |
| 6. Coordinator | 1 | `saturate_coordinator_actor.hpp` | — |
| 7. CLI Commands | 1 | `saturate_commands.cpp` | — |
| 8. Main | 1 | `17_bench_saturate.cpp` | — |
| 9. Verification | 1 | — | 24+ |

**Total: 11 tasks, 10 new files, 1 modified file, 24 unit tests.**
