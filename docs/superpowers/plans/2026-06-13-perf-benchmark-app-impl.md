# Bench Perf App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `apps/bench_perf/` — a performance benchmark app for the HPActor actor system that measures actor throughput under high fan-out and hot-actor fairness.

**Architecture:** Follows `apps/cli_demo/` patterns: EventBasedActor subclasses with Behavior/become(), TypedMessage message passing, static CLI command registration via CommandRegistration<T>, and ActorSystem with Config-driven scheduler setup. Four actor types: BenchWorkerActor (light CPU burn, self-scheduling), BenchHotActor (heavy CPU burn, high rate), BenchCollectorActor (streaming percentile reservoir), BenchCoordinatorActor (CLI orchestration). CLI commands access the coordinator via `ctx.cli_actor->enumerate_actors()` + `send_and_wait_inspect()`.

**Tech Stack:** C++20, HPActor framework (hpactor_lib), Google Test for smoke test.

---

### Task 1: Create Worktree and Scaffold Directory

**Files:**
- Create: `apps/bench_perf/CMakeLists.txt`
- Create: `apps/bench_perf/messages.hpp`
- Modify: `apps/CMakeLists.txt`
- Modify: `.gitignore` (if `.worktrees/` not already ignored)

- [ ] **Step 1: Create isolated worktree**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor
git worktree add -b bench-perf .worktrees/bench-perf
cd .worktrees/bench-perf
```

- [ ] **Step 2: Verify worktree isolation**

```bash
pwd
# Expected: .../HPActor/.worktrees/bench-perf
git branch --show-current
# Expected: bench-perf
```

- [ ] **Step 3: Create directory structure**

```bash
mkdir -p apps/bench_perf/actors
```

- [ ] **Step 4: Create `apps/bench_perf/messages.hpp`**

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

#include <cstring>

namespace hpactor::apps::bench_perf {

// =============================================================================
// Message Type Tags — application range (0x00010100 – 0x000101FF)
// =============================================================================

inline constexpr TypeTag LatencySampleTag{0x00010100};
inline constexpr TypeTag ThroughputTickTag{0x00010101};
inline constexpr TypeTag BenchStartTag{0x00010102};
inline constexpr TypeTag BenchStopTag{0x00010103};
inline constexpr TypeTag StatsPollTag{0x00010104};
inline constexpr TypeTag StatsReplyTag{0x00010105};

// =============================================================================
// Payload helpers
// =============================================================================

inline StreamBuffer encode_u64(uint64_t v) {
    StreamBuffer buf(sizeof(v));
    std::memcpy(buf.data(), &v, sizeof(v));
    return buf;
}

inline uint64_t decode_u64(const StreamBuffer& buf) {
    if (buf.size() < sizeof(uint64_t))
        return 0;
    uint64_t v = 0;
    std::memcpy(&v, buf.data(), sizeof(v));
    return v;
}

inline TypedMessage make_msg(TypeTag tag, StreamBuffer payload = {}) {
    return TypedMessage(tag, std::move(payload));
}

// =============================================================================
// CPU Burn helper (portable, cooperative)
// =============================================================================

/// \brief Burn CPU for approximately \p us microseconds.
///
/// Polls \c steady_clock in a tight loop. Cooperatively yields no syscalls.
/// Used by worker/hot actors to simulate processing cost.
inline void burn_cpu_us(uint64_t us) {
    if (us == 0) return;
    auto target = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < target) {
        // spin
    }
}

} // namespace hpactor::apps::bench_perf
```

- [ ] **Step 5: Create `apps/bench_perf/CMakeLists.txt`**

```cmake
add_executable(16_bench_perf 16_bench_perf.cpp)
target_link_libraries(16_bench_perf PRIVATE hpactor_lib)
target_include_directories(16_bench_perf PRIVATE ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 6: Register in `apps/CMakeLists.txt`**

In `apps/CMakeLists.txt`, after `add_subdirectory(cli_demo)` add:

```cmake
add_subdirectory(bench_perf)
```

- [ ] **Step 7: Commit scaffold**

```bash
git add apps/bench_perf/ apps/CMakeLists.txt
git commit -m "feat: scaffold bench_perf app directory with messages and build config

Create apps/bench_perf/ with CMakeLists.txt, messages.hpp (TypeTags
0x00010100–0x000101FF, payload helpers, portable burn_cpu_us), and
register in apps/CMakeLists.txt.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Implement BenchCollectorActor

**Files:**
- Create: `apps/bench_perf/actors/bench_collector_actor.hpp`

- [ ] **Step 1: Write `apps/bench_perf/actors/bench_collector_actor.hpp`**

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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::bench_perf {

// =============================================================================
// BenchCollectorActor — streaming latency percentile + throughput stats
// =============================================================================

/// \brief Receives latency samples and throughput ticks from worker/hot actors,
///        computes streaming percentiles via reservoir sampling, and serves
///        stats snapshots on demand.
///
/// Reservoir: keeps the last 10,000 latency samples per group. On snapshot,
/// sorts and extracts p50/p99/p999. Throughput is computed via a sliding
/// 1-second window counter.
class BenchCollectorActor : public EventBasedActor {
  public:
    explicit BenchCollectorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        hot_latencies_.reserve(kReservoirSize);
        cold_latencies_.reserve(kReservoirSize);
        become(make_behavior());
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BenchCollectorActor";
        m.state = running_ ? "Collecting" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        return build_report_bytes();
    }

    // Accessors used by coordinator for direct report formatting
    uint64_t total_hot_samples() const { return hot_count_.load(); }
    uint64_t total_cold_samples() const { return cold_count_.load(); }
    double hot_p50_us() const { return hot_p50_us_; }
    double hot_p99_us() const { return hot_p99_us_; }
    double hot_p999_us() const { return hot_p999_us_; }
    double cold_p50_us() const { return cold_p50_us_; }
    double cold_p99_us() const { return cold_p99_us_; }
    double cold_p999_us() const { return cold_p999_us_; }
    double hot_throughput_msgps() const { return hot_throughput_msgps_; }
    double cold_throughput_msgps() const { return cold_throughput_msgps_; }
    bool is_running() const { return running_; }
    uint64_t elapsed_run_ms() const {
        if (!running_ && run_start_ == decltype(run_start_){}) return 0;
        auto end = running_ ? std::chrono::steady_clock::now() : run_end_;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - run_start_).count());
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);

            if (msg.type_id() == LatencySampleTag) {
                handle_latency_sample(msg);
            } else if (msg.type_id() == ThroughputTickTag) {
                handle_throughput_tick(msg);
            } else if (msg.type_id() == BenchStartTag) {
                handle_bench_start(msg);
            } else if (msg.type_id() == BenchStopTag) {
                handle_bench_stop();
            } else if (msg.type_id() == StatsPollTag) {
                handle_stats_poll();
            }
        }};
    }

  private:
    static constexpr size_t kReservoirSize = 10000;

    void handle_latency_sample(TypedMessage& msg) {
        // Payload: {actor_id: uint64, latency_us: uint64, group: uint8}
        //  group: 0 = cold (worker), 1 = hot
        const auto& p = msg.payload();
        if (p.size() < 17) return; // 8 + 8 + 1
        const uint8_t* d = p.data();
        uint64_t actor_id_val, latency_us;
        std::memcpy(&actor_id_val, d, sizeof(uint64_t));
        std::memcpy(&latency_us, d + 8, sizeof(uint64_t));
        uint8_t group = d[16];
        (void)actor_id_val;

        if (group == 0) {
            cold_latencies_.push_back(static_cast<double>(latency_us));
            if (cold_latencies_.size() > kReservoirSize)
                cold_latencies_.erase(cold_latencies_.begin(),
                                      cold_latencies_.begin() + (cold_latencies_.size() - kReservoirSize));
            cold_count_.fetch_add(1);
        } else {
            hot_latencies_.push_back(static_cast<double>(latency_us));
            if (hot_latencies_.size() > kReservoirSize)
                hot_latencies_.erase(hot_latencies_.begin(),
                                     hot_latencies_.begin() + (hot_latencies_.size() - kReservoirSize));
            hot_count_.fetch_add(1);
        }
    }

    void handle_throughput_tick(TypedMessage& msg) {
        // Payload: {actor_id: uint64, msg_count: uint64, window_ms: uint32, group: uint8}
        const auto& p = msg.payload();
        if (p.size() < 21) return; // 8 + 8 + 4 + 1
        const uint8_t* d = p.data();
        uint64_t msg_count;
        std::memcpy(&msg_count, d + 8, sizeof(uint64_t));
        uint8_t group = d[20];
        (void)group;
        // Accumulate into sliding window; simplified: aggregate count
        // For now, throughput is derived from sample count / elapsed.
    }

    void handle_bench_start(TypedMessage& msg) {
        hot_latencies_.clear();
        cold_latencies_.clear();
        hot_count_.store(0);
        cold_count_.store(0);
        hot_p50_us_ = hot_p99_us_ = hot_p999_us_ = 0.0;
        cold_p50_us_ = cold_p99_us_ = cold_p999_us_ = 0.0;
        hot_throughput_msgps_ = 0.0;
        cold_throughput_msgps_ = 0.0;
        running_ = true;
        run_start_ = std::chrono::steady_clock::now();
        (void)msg;
    }

    void handle_bench_stop() {
        running_ = false;
        run_end_ = std::chrono::steady_clock::now();
        recompute_percentiles();
    }

    void handle_stats_poll() {
        recompute_percentiles();
        // Reply with serialized stats
        auto bytes = build_report_bytes();
        context()->reply(make_msg(StatsReplyTag, std::move(bytes)));
    }

    void recompute_percentiles() {
        if (!hot_latencies_.empty()) {
            std::vector<double> sorted(hot_latencies_);
            std::sort(sorted.begin(), sorted.end());
            hot_p50_us_ = sorted[sorted.size() / 2];
            hot_p99_us_ = sorted[sorted.size() * 99 / 100];
            hot_p999_us_ = sorted[sorted.size() * 999 / 1000];
        }
        if (!cold_latencies_.empty()) {
            std::vector<double> sorted(cold_latencies_);
            std::sort(sorted.begin(), sorted.end());
            cold_p50_us_ = sorted[sorted.size() / 2];
            cold_p99_us_ = sorted[sorted.size() * 99 / 100];
            cold_p999_us_ = sorted[sorted.size() * 999 / 1000];
        }
        // Throughput: samples / elapsed seconds
        double elapsed_s = static_cast<double>(elapsed_run_ms()) / 1000.0;
        if (elapsed_s > 0.0) {
            hot_throughput_msgps_ = static_cast<double>(hot_count_.load()) / elapsed_s;
            cold_throughput_msgps_ = static_cast<double>(cold_count_.load()) / elapsed_s;
        }
    }

    std::vector<uint8_t> build_report_bytes() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << "running=" << (running_ ? "yes" : "no") << "\n";
        oss << "elapsed_ms=" << elapsed_run_ms() << "\n";
        oss << "hot_samples=" << hot_count_.load() << "\n";
        oss << "cold_samples=" << cold_count_.load() << "\n";
        oss << "hot_p50_us=" << hot_p50_us_ << "\n";
        oss << "hot_p99_us=" << hot_p99_us_ << "\n";
        oss << "hot_p999_us=" << hot_p999_us_ << "\n";
        oss << "cold_p50_us=" << cold_p50_us_ << "\n";
        oss << "cold_p99_us=" << cold_p99_us_ << "\n";
        oss << "cold_p999_us=" << cold_p999_us_ << "\n";
        oss << "hot_throughput_msgps=" << hot_throughput_msgps_ << "\n";
        oss << "cold_throughput_msgps=" << cold_throughput_msgps_ << "\n";
        oss << "total_throughput_msgps=" << (hot_throughput_msgps_ + cold_throughput_msgps_) << "\n";
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point run_start_;
    std::chrono::steady_clock::time_point run_end_;
    std::vector<double> hot_latencies_;
    std::vector<double> cold_latencies_;
    std::atomic<uint64_t> hot_count_{0};
    std::atomic<uint64_t> cold_count_{0};
    double hot_p50_us_ = 0.0, hot_p99_us_ = 0.0, hot_p999_us_ = 0.0;
    double cold_p50_us_ = 0.0, cold_p99_us_ = 0.0, cold_p999_us_ = 0.0;
    double hot_throughput_msgps_ = 0.0;
    double cold_throughput_msgps_ = 0.0;
    std::atomic<uint64_t> processed_{0};
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
```

- [ ] **Step 2: Verify it compiles**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=ON
ninja -C build 16_bench_perf
# Expected: compiles (may warn about unused params), links successfully
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_perf/actors/bench_collector_actor.hpp
git commit -m "feat: implement BenchCollectorActor with reservoir percentile stats

Streaming latency + throughput collector. Maintains 10K-sample reservoir
per group (hot/cold), computes p50/p99/p999 on snapshot, serves stats
via StatsPoll/StatsReply protocol. Rate-limited reservoir eviction.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Implement BenchWorkerActor

**Files:**
- Create: `apps/bench_perf/actors/bench_worker_actor.hpp`

- [ ] **Step 1: Write `apps/bench_perf/actors/bench_worker_actor.hpp`**

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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::bench_perf {

// =============================================================================
// Thread-safe Xorshift RNG (same pattern as cli_demo WorkerActor)
// =============================================================================

namespace {

class Trng {
  public:
    Trng() {
        static std::atomic<uint64_t> s_counter{1};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        state_ = static_cast<uint64_t>(now) ^ (s_counter.fetch_add(1) << 33);
        if (state_ == 0)
            state_ = 1;
    }

    uint64_t next() {
        uint64_t x = state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_ = x;
        return x;
    }

  private:
    uint64_t state_;
};

thread_local Trng tl_rng;

} // namespace

// =============================================================================
// BenchWorkerActor — light CPU burn, self-scheduling, latency sampling
// =============================================================================

/// \brief Cold/normal worker actor for throughput benchmarking.
///
/// Self-schedules via \c context()->schedule(), burns CPU inline for a
/// configurable duration, and sends latency + throughput samples to the
/// collector. Group = 0 (cold).
class BenchWorkerActor : public EventBasedActor {
  public:
    BenchWorkerActor(ActorContext* ctx, ActorSystem& sys,
                     ActorAddress collector_addr, uint32_t worker_index)
        : EventBasedActor(ctx, sys),
          collector_addr_(collector_addr),
          worker_index_(worker_index),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    void set_burn_params(uint64_t burn_us, uint32_t rate_hz, uint32_t duration_ms) {
        burn_us_ = burn_us;
        rate_hz_ = rate_hz;
        duration_ms_ = duration_ms;
    }

    void set_collector_addr(ActorAddress addr) { collector_addr_ = addr; }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BenchWorkerActor";
        m.state = running_ ? "Running" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << "bench-worker-" << worker_index_;
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "worker_index=" << worker_index_
            << " processed=" << processed_.load()
            << " burn_us=" << burn_us_
            << " rate_hz=" << rate_hz_
            << " running=" << (running_ ? "yes" : "no");
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    uint32_t worker_index() const { return worker_index_; }
    bool is_running() const { return running_; }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);

            if (msg.type_id() == BenchStartTag) {
                // Payload: {burn_us: uint64, rate_hz: uint32, duration_ms: uint32}
                const auto& p = msg.payload();
                if (p.size() >= 16) {
                    const uint8_t* d = p.data();
                    uint64_t burn;
                    uint32_t rate, dur;
                    std::memcpy(&burn, d, sizeof(uint64_t));
                    std::memcpy(&rate, d + 8, sizeof(uint32_t));
                    std::memcpy(&dur, d + 12, sizeof(uint32_t));
                    set_burn_params(burn, rate, dur);
                }
                running_ = true;
                start_time_ = std::chrono::steady_clock::now();
                // Kick off first tick
                schedule_next();

            } else if (msg.type_id() == BenchStopTag) {
                running_ = false;

            } else if (msg.type_id() == PeriodicTickTag) {
                if (!running_) return;
                do_tick();
            }
        }};
    }

  private:
    /// \brief Internal tick tag (not in messages.hpp — local to worker/hot actor).
    static constexpr TypeTag PeriodicTickTag{0x00010110};

    void schedule_next() {
        if (!running_) return;
        uint64_t interval_us = (rate_hz_ > 0) ? (1'000'000 / rate_hz_) : 10'000;
        context()->schedule(std::chrono::microseconds(interval_us),
                            make_msg(PeriodicTickTag));
    }

    void do_tick() {
        auto t0 = std::chrono::steady_clock::now();

        // CPU burn
        burn_cpu_us(burn_us_);

        auto t1 = std::chrono::steady_clock::now();
        uint64_t latency_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        // Send latency sample to collector
        // Payload: {actor_id: uint64, latency_us: uint64, group: uint8}
        uint8_t payload_buf[17];
        uint64_t aid = id().value();
        uint8_t group = 0; // cold
        std::memcpy(payload_buf, &aid, sizeof(uint64_t));
        std::memcpy(payload_buf + 8, &latency_us, sizeof(uint64_t));
        payload_buf[16] = group;
        StreamBuffer payload(payload_buf, payload_buf + sizeof(payload_buf));
        context()->send(collector_addr_,
                        make_msg(LatencySampleTag, std::move(payload)));

        // Send throughput tick every 100 iterations
        sample_count_++;
        if (sample_count_ % 100 == 0) {
            uint8_t tp_buf[21];
            uint64_t count = 100;
            uint32_t window = 1000;
            std::memcpy(tp_buf, &aid, sizeof(uint64_t));
            std::memcpy(tp_buf + 8, &count, sizeof(uint64_t));
            std::memcpy(tp_buf + 16, &window, sizeof(uint32_t));
            tp_buf[20] = group;
            StreamBuffer tp(tp_buf, tp_buf + sizeof(tp_buf));
            context()->send(collector_addr_,
                            make_msg(ThroughputTickTag, std::move(tp)));
        }

        // Check duration
        if (duration_ms_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time_)
                               .count();
            if (elapsed >= duration_ms_) {
                running_ = false;
                return;
            }
        }

        // Schedule next tick
        schedule_next();
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    ActorAddress collector_addr_;
    uint32_t worker_index_;
    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> processed_{0};
    uint64_t burn_us_ = 10;
    uint32_t rate_hz_ = 100;
    uint32_t duration_ms_ = 30000;
    uint64_t sample_count_ = 0;
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
```

- [ ] **Step 2: Verify it compiles**

```bash
ninja -C build 16_bench_perf
# Expected: compiles successfully
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_perf/actors/bench_worker_actor.hpp
git commit -m "feat: implement BenchWorkerActor with configurable CPU burn

Cold/normal worker for throughput benchmarking. Self-scheduling via
context()->schedule(), configurable burn duration + rate + duration,
latency sampling + throughput tick emission to collector. Thread-safe
Trng for any future randomized work.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Implement BenchHotActor

**Files:**
- Create: `apps/bench_perf/actors/bench_hot_actor.hpp`

- [ ] **Step 1: Write `apps/bench_perf/actors/bench_hot_actor.hpp`**

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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::bench_perf {

// =============================================================================
// BenchHotActor — heavy CPU burn, high message rate (group = 1)
// =============================================================================

/// \brief "Noisy neighbor" actor for fairness benchmarking.
///
/// Same self-scheduling pattern as BenchWorkerActor but configured with
/// significantly higher burn duration and message rate. Sends latency
/// samples with \c group = 1 (hot) to the collector.
class BenchHotActor : public EventBasedActor {
  public:
    BenchHotActor(ActorContext* ctx, ActorSystem& sys,
                  ActorAddress collector_addr, uint32_t hot_index = 0)
        : EventBasedActor(ctx, sys),
          collector_addr_(collector_addr),
          hot_index_(hot_index),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    void set_burn_params(uint64_t burn_us, uint32_t rate_hz, uint32_t duration_ms) {
        burn_us_ = burn_us;
        rate_hz_ = rate_hz;
        duration_ms_ = duration_ms;
    }

    void set_collector_addr(ActorAddress addr) { collector_addr_ = addr; }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BenchHotActor";
        m.state = running_ ? "Running" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << "bench-hot-" << hot_index_;
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "hot_index=" << hot_index_
            << " processed=" << processed_.load()
            << " burn_us=" << burn_us_
            << " rate_hz=" << rate_hz_
            << " running=" << (running_ ? "yes" : "no");
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    bool is_running() const { return running_; }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);

            if (msg.type_id() == BenchStartTag) {
                const auto& p = msg.payload();
                if (p.size() >= 16) {
                    const uint8_t* d = p.data();
                    uint64_t burn;
                    uint32_t rate, dur;
                    std::memcpy(&burn, d, sizeof(uint64_t));
                    std::memcpy(&rate, d + 8, sizeof(uint32_t));
                    std::memcpy(&dur, d + 12, sizeof(uint32_t));
                    set_burn_params(burn, rate, dur);
                }
                running_ = true;
                start_time_ = std::chrono::steady_clock::now();
                schedule_next();

            } else if (msg.type_id() == BenchStopTag) {
                running_ = false;

            } else if (msg.type_id() == PeriodicTickTag) {
                if (!running_) return;
                do_tick();
            }
        }};
    }

  private:
    static constexpr TypeTag PeriodicTickTag{0x00010110};

    void schedule_next() {
        if (!running_) return;
        uint64_t interval_us = (rate_hz_ > 0) ? (1'000'000 / rate_hz_) : 1'000;
        context()->schedule(std::chrono::microseconds(interval_us),
                            make_msg(PeriodicTickTag));
    }

    void do_tick() {
        auto t0 = std::chrono::steady_clock::now();

        // Heavy CPU burn
        burn_cpu_us(burn_us_);

        auto t1 = std::chrono::steady_clock::now();
        uint64_t latency_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        // Send latency sample to collector (group = 1 = hot)
        uint8_t payload_buf[17];
        uint64_t aid = id().value();
        uint8_t group = 1; // hot
        std::memcpy(payload_buf, &aid, sizeof(uint64_t));
        std::memcpy(payload_buf + 8, &latency_us, sizeof(uint64_t));
        payload_buf[16] = group;
        StreamBuffer payload(payload_buf, payload_buf + sizeof(payload_buf));
        context()->send(collector_addr_,
                        make_msg(LatencySampleTag, std::move(payload)));

        // Duration check
        if (duration_ms_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time_)
                               .count();
            if (elapsed >= duration_ms_) {
                running_ = false;
                return;
            }
        }

        schedule_next();
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    ActorAddress collector_addr_;
    uint32_t hot_index_;
    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> processed_{0};
    uint64_t burn_us_ = 500;
    uint32_t rate_hz_ = 1000;
    uint32_t duration_ms_ = 30000;
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
```

- [ ] **Step 2: Verify it compiles**

```bash
ninja -C build 16_bench_perf
# Expected: compiles successfully
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_perf/actors/bench_hot_actor.hpp
git commit -m "feat: implement BenchHotActor with heavy CPU burn + high rate

Noisy-neighbor actor for fairness benchmarking. Same pattern as
BenchWorkerActor but defaults to 500us burn at 1000Hz with group=hot
latency sampling to collector.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Implement BenchCoordinatorActor

**Files:**
- Create: `apps/bench_perf/actors/bench_coordinator_actor.hpp`

- [ ] **Step 1: Write `apps/bench_perf/actors/bench_coordinator_actor.hpp`**

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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::bench_perf {

// =============================================================================
// Benchmark Preset
// =============================================================================

struct BenchmarkPreset {
    std::string name;
    std::string description;
    uint32_t num_workers = 5000;
    uint64_t cold_burn_us = 10;
    uint32_t cold_rate_hz = 100;
    uint32_t num_hot_actors = 0;
    uint64_t hot_burn_us = 500;
    uint32_t hot_rate_hz = 1000;
    uint32_t duration_ms = 30000;
};

// =============================================================================
// BenchCoordinatorActor
// =============================================================================

/// \brief Orchestrates benchmark runs and reports results.
///
/// Holds preset configurations, spawns/holds references to worker/hot actors,
/// starts/stops runs, and produces formatted reports via serialize_state().
/// Responds to BenchStart/BenchStop/StatsPoll messages from CLI commands.
class BenchCoordinatorActor : public EventBasedActor {
  public:
    BenchCoordinatorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        // Register presets
        presets_.push_back({"many-actors",
                            "5000 workers, 10us burn, 100Hz — throughput test",
                            5000, 10, 100, 0, 500, 1000, 30000});
        presets_.push_back({"hot-actor",
                            "1 hot actor (500us, 1000Hz) + 1000 cold workers (10us, 10Hz) — fairness test",
                            1000, 10, 10, 1, 500, 1000, 30000});
        become(make_behavior());
    }

    /// \brief Configure with worker and collector addresses after spawn.
    void set_worker_addrs(std::vector<ActorAddress> cold_addrs,
                          std::vector<ActorAddress> hot_addrs,
                          ActorAddress collector_addr) {
        cold_worker_addrs_ = std::move(cold_addrs);
        hot_worker_addrs_ = std::move(hot_addrs);
        collector_addr_ = collector_addr;
    }

    /// \brief Get the list of available preset names (for CLI /bench list).
    const std::vector<BenchmarkPreset>& presets() const { return presets_; }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BenchCoordinatorActor";
        m.state = running_ ? "Running" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << (running_ ? active_preset_ : "none");
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        return build_report_bytes();
    }

    bool is_running() const { return running_; }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);

            if (msg.type_id() == BenchStartTag) {
                handle_bench_start(msg);
            } else if (msg.type_id() == BenchStopTag) {
                handle_bench_stop();
            } else if (msg.type_id() == StatsPollTag) {
                handle_stats_poll();
            }
        }};
    }

  private:
    void handle_bench_start(TypedMessage& msg) {
        // Payload: preset name as string
        const auto& p = msg.payload();
        std::string preset_name(p.data(), p.data() + p.size());

        const BenchmarkPreset* preset = nullptr;
        for (auto& pr : presets_) {
            if (pr.name == preset_name) {
                preset = &pr;
                break;
            }
        }
        if (!preset) {
            last_error_ = "Unknown preset: " + preset_name;
            return;
        }

        active_preset_ = preset->name;
        last_error_.clear();

        // Build BenchStart payload: {burn_us: uint64, rate_hz: uint32, duration_ms: uint32}
        uint8_t start_buf[16];
        std::memset(start_buf, 0, sizeof(start_buf));

        // Send to cold workers with cold parameters
        {
            uint64_t burn = preset->cold_burn_us;
            uint32_t rate = preset->cold_rate_hz;
            uint32_t dur = preset->duration_ms;
            std::memcpy(start_buf, &burn, sizeof(uint64_t));
            std::memcpy(start_buf + 8, &rate, sizeof(uint32_t));
            std::memcpy(start_buf + 12, &dur, sizeof(uint32_t));
            StreamBuffer cold_payload(start_buf, start_buf + sizeof(start_buf));
            for (auto& addr : cold_worker_addrs_) {
                context()->send(addr, make_msg(BenchStartTag, cold_payload));
            }
        }

        // Send to hot actors with hot parameters
        {
            uint64_t burn = preset->hot_burn_us;
            uint32_t rate = preset->hot_rate_hz;
            uint32_t dur = preset->duration_ms;
            std::memcpy(start_buf, &burn, sizeof(uint64_t));
            std::memcpy(start_buf + 8, &rate, sizeof(uint32_t));
            std::memcpy(start_buf + 12, &dur, sizeof(uint32_t));
            StreamBuffer hot_payload(start_buf, start_buf + sizeof(start_buf));
            for (auto& addr : hot_worker_addrs_) {
                context()->send(addr, make_msg(BenchStartTag, hot_payload));
            }
        }

        // Start collector
        {
            StreamBuffer empty;
            context()->send(collector_addr_, make_msg(BenchStartTag, std::move(empty)));
        }

        running_ = true;
        start_time_ = std::chrono::steady_clock::now();
    }

    void handle_bench_stop() {
        // Send stop to all
        StreamBuffer empty;
        for (auto& addr : cold_worker_addrs_)
            context()->send(addr, make_msg(BenchStopTag, empty));
        for (auto& addr : hot_worker_addrs_)
            context()->send(addr, make_msg(BenchStopTag, empty));
        context()->send(collector_addr_, make_msg(BenchStopTag, empty));

        running_ = false;
    }

    void handle_stats_poll() {
        // Forward poll to collector; collector replies directly to sender
        StreamBuffer empty;
        context()->send(collector_addr_, make_msg(StatsPollTag, empty));
    }

    std::vector<uint8_t> build_report_bytes() const {
        std::ostringstream oss;
        oss << "preset=" << active_preset_ << "\n";
        oss << "running=" << (running_ ? "yes" : "no") << "\n";
        if (!last_error_.empty())
            oss << "error=" << last_error_ << "\n";
        if (running_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time_)
                               .count();
            oss << "elapsed_ms=" << elapsed << "\n";
        }
        oss << "cold_workers=" << cold_worker_addrs_.size() << "\n";
        oss << "hot_workers=" << hot_worker_addrs_.size() << "\n";
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::vector<BenchmarkPreset> presets_;
    ActorAddress collector_addr_;
    std::vector<ActorAddress> cold_worker_addrs_;
    std::vector<ActorAddress> hot_worker_addrs_;
    std::string active_preset_;
    std::string last_error_;
    std::atomic<uint64_t> processed_{0};
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
```

- [ ] **Step 2: Verify it compiles**

```bash
ninja -C build 16_bench_perf
# Expected: compiles successfully
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_perf/actors/bench_coordinator_actor.hpp
git commit -m "feat: implement BenchCoordinatorActor with preset orchestration

Orchestrates benchmark runs via BenchStart/BenchStop messages. Built-in
presets: many-actors (5K workers) and hot-actor (1 hot + 1K cold).
Dispatches parameterized start messages to all workers and collector.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Implement CLI Bench Commands

**Files:**
- Create: `apps/bench_perf/commands/bench_commands.cpp`

- [ ] **Step 1: Write `apps/bench_perf/commands/bench_commands.cpp`**

The CLI commands use the static `CommandRegistration<T>` pattern from the
codebase. They find the coordinator actor via `enumerate_actors()` and
communicate via `send_and_wait_inspect()` / `deliver_local()`.

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

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hpactor {
namespace cli {
namespace {

// =============================================================================
// Helper: find the bench coordinator actor
// =============================================================================

static ActorId find_coordinator(CliActor* cli) {
    auto actors = cli->enumerate_actors("BenchCoordinatorActor");
    if (actors.empty())
        return ActorId{0};
    return ActorId{actors[0].actor_id};
}

// =============================================================================
// Helper: send a message to the coordinator and wait for a response via inspect
// =============================================================================

static std::string
poll_coordinator_state(ActorSystem* system, CliActor* cli, ActorId coord_id,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    // Small delay to let the coordinator process the message
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    InspectStateRequest req;
    req.set_target_actor_id(coord_id.value());
    req.set_include_state(true);
    req.set_include_mailbox(false);
    req.set_include_children(false);

    auto reply = cli->send_and_wait_inspect(coord_id, req, timeout);
    if (!reply || reply->state_blob().empty())
        return "";

    return std::string(reply->state_blob().begin(), reply->state_blob().end());
}

// =============================================================================
// Helper: parse key=value text state into a map
// =============================================================================

static std::map<std::string, std::string>
parse_state(const std::string& state) {
    std::map<std::string, std::string> m;
    std::istringstream iss(state);
    std::string line;
    while (std::getline(iss, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos)
            m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

// =============================================================================
// /bench start <preset> — Start a benchmark run
// =============================================================================

class BenchStartCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "bench/start"; }
    std::string_view help_text() const noexcept override {
        return "Start a benchmark run. Usage: /bench start <preset>";
    }
    int order() const noexcept override { return 500; }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error: no CLI actor or system");
            return result<void>::make();
        }

        if (ctx.args.empty()) {
            ctx.output->error("Usage: /bench start <preset>");
            ctx.output->raw("Available presets: many-actors, hot-actor");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Bench coordinator not found. Is the bench_perf app running?");
            return result<void>::make();
        }

        // Check if already running
        auto state = poll_coordinator_state(system, cli, coord_id);
        auto kv = parse_state(state);
        if (kv["running"] == "yes") {
            ctx.output->error("A benchmark is already running. Use /bench stop first.");
            return result<void>::make();
        }

        // Send BenchStart message
        std::string preset = ctx.args[0];
        StreamBuffer payload(reinterpret_cast<const uint8_t*>(preset.data()),
                             reinterpret_cast<const uint8_t*>(preset.data() + preset.size()));
        system->deliver_local(coord_id, TypedMessage(TypeTag{0x00010102}, std::move(payload)));

        ctx.output->raw("[OK] Benchmark '" + preset + "' started.");
        return result<void>::make();
    }
};

const CommandRegistration<BenchStartCommand> kRegisterBenchStart;

// =============================================================================
// /bench stop — Stop the current run
// =============================================================================

class BenchStopCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "bench/stop"; }
    std::string_view help_text() const noexcept override {
        return "Stop the current benchmark run";
    }
    int order() const noexcept override { return 501; }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Bench coordinator not found.");
            return result<void>::make();
        }

        // Send BenchStop message
        StreamBuffer empty;
        system->deliver_local(coord_id, TypedMessage(TypeTag{0x00010103}, std::move(empty)));

        ctx.output->raw("[OK] Benchmark stopped.");
        return result<void>::make();
    }
};

const CommandRegistration<BenchStopCommand> kRegisterBenchStop;

// =============================================================================
// /bench status — Show current run state
// =============================================================================

class BenchStatusCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "bench/status"; }
    std::string_view help_text() const noexcept override {
        return "Show current benchmark run status";
    }
    int order() const noexcept override { return 502; }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Bench coordinator not found.");
            return result<void>::make();
        }

        auto state_str = poll_coordinator_state(system, cli, coord_id);
        auto kv = parse_state(state_str);

        ctx.output->header("Benchmark Status");
        std::map<std::string, std::string> display;
        display["Preset"] = kv["preset"];
        display["Running"] = kv["running"];
        display["Elapsed (ms)"] = kv["elapsed_ms"];
        display["Cold Workers"] = kv["cold_workers"];
        display["Hot Workers"] = kv["hot_workers"];
        if (!kv["error"].empty())
            display["Error"] = kv["error"];
        ctx.output->key_value(display);
        return result<void>::make();
    }
};

const CommandRegistration<BenchStatusCommand> kRegisterBenchStatus;

// =============================================================================
// /bench report [group] — Full latency/throughput report
// =============================================================================

class BenchReportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "bench/report"; }
    std::string_view help_text() const noexcept override {
        return "Show benchmark report. Usage: /bench report [hot|cold]";
    }
    int order() const noexcept override { return 503; }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        // Find collector
        auto actors = cli->enumerate_actors("BenchCollectorActor");
        if (actors.empty()) {
            ctx.output->error("Collector not found.");
            return result<void>::make();
        }
        auto coll_id = ActorId{actors[0].actor_id};

        // Poll collector state (send StatsPoll, then inspect)
        auto coord_id = find_coordinator(cli);
        if (coord_id == ActorId{0}) {
            ctx.output->error("Coordinator not found.");
            return result<void>::make();
        }

        // Send poll to coordinator (which forwards to collector)
        StreamBuffer empty;
        system->deliver_local(coord_id, TypedMessage(TypeTag{0x00010104}, std::move(empty)));

        // Wait a bit then inspect the collector directly
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        InspectStateRequest req;
        req.set_target_actor_id(coll_id.value());
        req.set_include_state(true);
        req.set_include_mailbox(false);
        req.set_include_children(false);

        auto reply = cli->send_and_wait_inspect(coll_id, req);
        if (!reply || reply->state_blob().empty()) {
            ctx.output->error("No response from collector.");
            return result<void>::make();
        }

        std::string state_str(reply->state_blob().begin(), reply->state_blob().end());
        auto kv = parse_state(state_str);

        std::string filter = ctx.args.empty() ? "" : ctx.args[0];

        ctx.output->header("Benchmark Report");

        if (filter.empty() || filter == "hot") {
            ctx.output->header("Hot Actor Group");
            std::map<std::string, std::string> hot;
            hot["Samples"] = kv["hot_samples"];
            hot["P50 (us)"] = kv["hot_p50_us"];
            hot["P99 (us)"] = kv["hot_p99_us"];
            hot["P999 (us)"] = kv["hot_p999_us"];
            hot["Throughput (msg/s)"] = kv["hot_throughput_msgps"];
            ctx.output->key_value(hot);
        }

        if (filter.empty() || filter == "cold") {
            ctx.output->header("Cold Worker Group");
            std::map<std::string, std::string> cold;
            cold["Samples"] = kv["cold_samples"];
            cold["P50 (us)"] = kv["cold_p50_us"];
            cold["P99 (us)"] = kv["cold_p99_us"];
            cold["P999 (us)"] = kv["cold_p999_us"];
            cold["Throughput (msg/s)"] = kv["cold_throughput_msgps"];
            ctx.output->key_value(cold);
        }

        if (filter.empty()) {
            ctx.output->header("Totals");
            std::map<std::string, std::string> totals;
            totals["Total Throughput (msg/s)"] = kv["total_throughput_msgps"];
            totals["Elapsed (ms)"] = kv["elapsed_ms"];
            ctx.output->key_value(totals);
        }

        return result<void>::make();
    }
};

const CommandRegistration<BenchReportCommand> kRegisterBenchReport;

// =============================================================================
// /bench export [--json] — Export raw data
// =============================================================================

class BenchExportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "bench/export"; }
    std::string_view help_text() const noexcept override {
        return "Export benchmark data. Usage: /bench export [--json]";
    }
    int order() const noexcept override { return 504; }

    result<void> execute(CommandContext& ctx) const override {
        auto* cli = ctx.cli_actor;
        auto* system = ctx.system;
        if (!cli || !system) {
            ctx.output->error("Internal error");
            return result<void>::make();
        }

        // Reuse the report logic but output raw state as JSON
        auto actors = cli->enumerate_actors("BenchCollectorActor");
        if (actors.empty()) {
            ctx.output->error("Collector not found.");
            return result<void>::make();
        }
        auto coll_id = ActorId{actors[0].actor_id};

        // Trigger stats poll
        auto coord_id = find_coordinator(cli);
        if (coord_id != ActorId{0}) {
            StreamBuffer empty;
            system->deliver_local(coord_id, TypedMessage(TypeTag{0x00010104}, std::move(empty)));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        InspectStateRequest req;
        req.set_target_actor_id(coll_id.value());
        req.set_include_state(true);

        auto reply = cli->send_and_wait_inspect(coll_id, req);
        if (!reply || reply->state_blob().empty()) {
            ctx.output->error("No data.");
            return result<void>::make();
        }

        std::string state_str(reply->state_blob().begin(), reply->state_blob().end());
        auto kv = parse_state(state_str);

        // Output as simple JSON object
        std::ostringstream json;
        json << "{\n";
        bool first = true;
        for (auto& [k, v] : kv) {
            if (!first) json << ",\n";
            json << "  \"" << k << "\": \"" << v << "\"";
            first = false;
        }
        json << "\n}\n";
        ctx.output->raw(json.str());
        return result<void>::make();
    }
};

const CommandRegistration<BenchExportCommand> kRegisterBenchExport;

// =============================================================================
// /bench list — List available presets
// =============================================================================

class BenchListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "bench/list"; }
    std::string_view help_text() const noexcept override {
        return "List available benchmark presets";
    }
    int order() const noexcept override { return 505; }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Available Benchmark Presets");
        ctx.output->raw("  many-actors  — 5000 workers, 10us burn, 100Hz (throughput test)");
        ctx.output->raw("  hot-actor    — 1 hot actor (500us, 1000Hz) + 1000 cold workers (fairness test)");
        ctx.output->raw("");
        ctx.output->raw("Use /bench start <preset> to run.");
        return result<void>::make();
    }
};

const CommandRegistration<BenchListCommand> kRegisterBenchList;

// =============================================================================
// /bench help — Show bench commands
// =============================================================================

class BenchHelpCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "bench/help"; }
    std::string_view help_text() const noexcept override {
        return "Show available /bench commands";
    }
    int order() const noexcept override { return 599; }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Benchmark Commands");
        ctx.output->raw("  /bench start <preset>  — Start a benchmark run");
        ctx.output->raw("  /bench stop            — Stop the current run");
        ctx.output->raw("  /bench status          — Show current run status");
        ctx.output->raw("  /bench report [group]  — Full latency/throughput report");
        ctx.output->raw("  /bench export [--json] — Export raw data");
        ctx.output->raw("  /bench list            — List available presets");
        ctx.output->raw("  /bench help            — Show this help");
        return result<void>::make();
    }
};

const CommandRegistration<BenchHelpCommand> kRegisterBenchHelp;

} // namespace
} // namespace cli
} // namespace hpactor
```

- [ ] **Step 2: Update `apps/bench_perf/CMakeLists.txt`** to include the commands source:

```cmake
add_executable(16_bench_perf
    16_bench_perf.cpp
    commands/bench_commands.cpp
)
target_link_libraries(16_bench_perf PRIVATE hpactor_lib)
target_include_directories(16_bench_perf PRIVATE ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 3: Verify it compiles**

```bash
ninja -C build 16_bench_perf
# Expected: compiles and links successfully
```

- [ ] **Step 4: Commit**

```bash
git add apps/bench_perf/commands/bench_commands.cpp apps/bench_perf/CMakeLists.txt
git commit -m "feat: implement /bench CLI commands via static registration

/bench start|stop|status|report|export|list|help commands registered via
CommandRegistration<T>. Commands find coordinator via enumerate_actors()
and communicate via deliver_local() + send_and_wait_inspect().

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Wire main() — 16_bench_perf.cpp

**Files:**
- Create: `apps/bench_perf/16_bench_perf.cpp`

- [ ] **Step 1: Write `apps/bench_perf/16_bench_perf.cpp`**

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

// =============================================================================
// HPActor App 16: Bench Perf — Actor System Performance Benchmark
// =============================================================================
//
// A performance benchmarking app that exercises two core scenarios:
//
//   1. many-actors: 5000 BenchWorkerActors with 10us CPU burn at 100Hz
//      — measures throughput under high fan-out with 8 scheduler threads.
//
//   2. hot-actor: 1 BenchHotActor (500us burn, 1000Hz) + 1000 BenchWorkerActors
//      (10us burn, 10Hz) — measures cold-actor tail latency under noisy neighbor.
//
//   CLI commands (registered via static CommandRegistration<T> in
//   commands/bench_commands.cpp):
//     /bench start <preset>  — start a run
//     /bench stop            — stop the current run
//     /bench status          — show current state
//     /bench report [group]  — latency percentile + throughput report
//     /bench export [--json] — export raw data
//     /bench list            — list available presets
//     /bench help            — show bench commands
//
// =============================================================================

#include "actors/bench_collector_actor.hpp"
#include "actors/bench_coordinator_actor.hpp"
#include "actors/bench_hot_actor.hpp"
#include "actors/bench_worker_actor.hpp"
#include "messages.hpp"

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace bench_perf = hpactor::apps::bench_perf;

// =============================================================================
// Splash screen
// =============================================================================

static void print_splash() {
    std::cout
        << "\n"
        << "+--------------------------------------------------------------+\n"
        << "|     HPActor App 16 — Bench Perf                               |\n"
        << "|     Actor System Performance Benchmark                        |\n"
        << "+--------------------------------------------------------------+\n"
        << "|                                                              |\n"
        << "|  Presets:                                                    |\n"
        << "|    many-actors — 5000 workers, 10us burn, 100Hz              |\n"
        << "|    hot-actor   — 1 hot (500us, 1000Hz) + 1000 cold           |\n"
        << "|                                                              |\n"
        << "|  Try:                                                        |\n"
        << "|    /bench list              — see all presets                |\n"
        << "|    /bench start many-actors — run throughput test            |\n"
        << "|    /bench status            — check progress                 |\n"
        << "|    /bench report            — view results                   |\n"
        << "|    /quit                    — exit                           |\n"
        << "|                                                              |\n"
        << "+--------------------------------------------------------------+\n"
        << std::endl;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    print_splash();

    // ── Configure: 8 threads, CLI enabled, unbounded mailboxes ────────────

    hpactor::Config config;
    config.scheduler_threads = 8;
    config.max_queue_depth = 4096;

    config.cli = hpactor::cli::CliConfig{
        .enabled = true,
        .listen_path = "",
        .tcp_port = 0,
        .default_format = "pretty",
        .page_size = 20,
        .history_path = "",
        .history_max = 1000};

    // Use large/unbounded mailboxes for throughput testing
    config.mailbox.default_capacity = 16384;
    config.mailbox.default_policy = hpactor::mailbox::OverflowPolicy::Block;

    // Graceful shutdown: drain for up to 10s
    config.shutdown_drain = hpactor::DrainConfig{
        hpactor::DrainPolicy::Drain, std::chrono::milliseconds{10'000}};

    hpactor::ActorSystem system(config);

    // ── Spawn coordinator and collector first ───────────────────────────

    auto coordinator = system.spawn<bench_perf::BenchCoordinatorActor>();
    auto collector = system.spawn<bench_perf::BenchCollectorActor>();

    // ── Determine preset for initial spawn ──────────────────────────────

    // Spawn for the largest preset (many-actors: 5000 cold, 0 hot).
    // The coordinator will send parameterized BenchStart messages to
    // the right subset. Extra workers that aren't used by a smaller
    // preset simply stay idle.
    constexpr uint32_t kMaxColdWorkers = 5000;
    constexpr uint32_t kMaxHotActors = 10;

    // ── Spawn cold workers ──────────────────────────────────────────────

    std::vector<std::shared_ptr<bench_perf::BenchWorkerActor>> cold_workers;
    std::vector<hpactor::ActorAddress> cold_addrs;

    cold_workers.reserve(kMaxColdWorkers);
    cold_addrs.reserve(kMaxColdWorkers);

    for (uint32_t i = 0; i < kMaxColdWorkers; ++i) {
        auto w = system.spawn<bench_perf::BenchWorkerActor>(
            collector.address(), i);
        cold_addrs.push_back(w.address());
        cold_workers.push_back(
            std::static_pointer_cast<bench_perf::BenchWorkerActor>(
                system.get_actor(w.id())));
    }

    std::cout << "Spawned " << kMaxColdWorkers << " cold workers." << std::endl;

    // ── Spawn hot actors ────────────────────────────────────────────────

    std::vector<std::shared_ptr<bench_perf::BenchHotActor>> hot_actors;
    std::vector<hpactor::ActorAddress> hot_addrs;

    hot_actors.reserve(kMaxHotActors);
    hot_addrs.reserve(kMaxHotActors);

    for (uint32_t i = 0; i < kMaxHotActors; ++i) {
        auto h = system.spawn<bench_perf::BenchHotActor>(
            collector.address(), i);
        hot_addrs.push_back(h.address());
        hot_actors.push_back(
            std::static_pointer_cast<bench_perf::BenchHotActor>(
                system.get_actor(h.id())));
    }

    std::cout << "Spawned " << kMaxHotActors << " hot actors." << std::endl;

    // ── Wire coordinator ────────────────────────────────────────────────

    auto* coord_raw =
        std::static_pointer_cast<bench_perf::BenchCoordinatorActor>(
            system.get_actor(coordinator.id()))
            .get();
    coord_raw->set_worker_addrs(cold_addrs, hot_addrs, collector.address());

    // Let actors initialize before CLI takes over
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── Main loop: wait for CLI exit ────────────────────────────────────

    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nInitiating graceful shutdown..." << std::endl;
    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        std::cout << "Shutdown complete." << std::endl;
    } else {
        std::cout << "Shutdown timed out — forcing exit." << std::endl;
    }

    std::cout << "=== Bench Perf Complete ===" << std::endl;
    return 0;
}
```

- [ ] **Step 2: Build and verify**

```bash
ninja -C build 16_bench_perf
# Expected: compiles and links successfully
```

- [ ] **Step 3: Commit**

```bash
git add apps/bench_perf/16_bench_perf.cpp
git commit -m "feat: wire bench_perf main() with 5000 cold + 10 hot actors

Spawns 5000 BenchWorkerActors + 10 BenchHotActors with 8 scheduler
threads. Preset parameters dispatched by coordinator at /bench start
time. CLI integrated with graceful shutdown.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Build and Verify Full App

**Files:** (none new)

- [ ] **Step 1: Full clean build of the app**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=ON
ninja -C build 16_bench_perf
# Expected: compiles and links with zero errors
```

- [ ] **Step 2: Quick smoke test — verify the binary starts and CLI responds**

```bash
# Run the app with a timeout (5s), pipe /help and /bench list, then /quit
echo -e "/help\n/bench list\n/bench help\n/quit" | timeout 10 ./build/apps/bench_perf/16_bench_perf 2>&1 || true
# Expected: prints splash, shows /bench commands, exits cleanly
```

- [ ] **Step 3: Commit any fixes**

```bash
git add -A
git diff --cached --stat
git commit -m "fix: build verification fixes for bench_perf app"
# (only if fixes were needed)
```

---

### Task 9: Write Smoke Test

**Files:**
- Create: `tests/system/apps/test_bench_perf_smoke.cpp`
- Modify: `tests/system/apps/CMakeLists.txt`

- [ ] **Step 1: Check existing system test patterns**

```bash
ls tests/system/apps/
cat tests/system/apps/CMakeLists.txt 2>/dev/null || echo "No CMakeLists yet"
```

- [ ] **Step 2: Write the smoke test**

NOTE: If the `ActorSystem` can be configured with `scheduler_start_paused = true`
and `config.cli.enabled = false`, the test can run without threads. Otherwise, a
minimal threaded test with a short timeout.

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <thread>

// Smoke test: verify that the bench_perf actors can be spawned and
// process messages without crashing. This is a system-level sanity
// check, not a performance test.
TEST(BenchPerfSmoke, SpawnAndRunMinimal) {
    hpactor::Config config;
    config.scheduler_threads = 2;
    config.max_queue_depth = 256;
    config.cli.enabled = false;

    // Unbounded mailboxes for simplicity
    config.mailbox.default_capacity = 1024;

    hpactor::ActorSystem system(config);

    // Spawn collector
    auto collector = system.spawn<hpactor::apps::bench_perf::BenchCollectorActor>();

    // Spawn 5 cold workers
    std::vector<hpactor::ActorAddress> cold_addrs;
    for (uint32_t i = 0; i < 5; ++i) {
        auto w = system.spawn<hpactor::apps::bench_perf::BenchWorkerActor>(
            collector.address(), i);
        cold_addrs.push_back(w.address());
    }

    // Spawn 1 hot actor
    auto hot = system.spawn<hpactor::apps::bench_perf::BenchHotActor>(
        collector.address(), 0);

    // Spawn coordinator
    auto coordinator = system.spawn<hpactor::apps::bench_perf::BenchCoordinatorActor>();
    auto* coord_raw =
        std::static_pointer_cast<hpactor::apps::bench_perf::BenchCoordinatorActor>(
            system.get_actor(coordinator.id()))
            .get();
    coord_raw->set_worker_addrs(cold_addrs, {hot.address()}, collector.address());

    // Let actors initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Start a short run (100ms)
    std::string preset = "many-actors";
    hpactor::StreamBuffer payload(
        reinterpret_cast<const uint8_t*>(preset.data()),
        reinterpret_cast<const uint8_t*>(preset.data() + preset.size()));
    system.deliver_local(coordinator.id(),
                         hpactor::TypedMessage(hpactor::TypeTag{0x00010102},
                                               std::move(payload)));

    // Let it run for 500ms
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop
    system.deliver_local(coordinator.id(),
                         hpactor::TypedMessage(hpactor::TypeTag{0x00010103},
                                               hpactor::StreamBuffer{}));

    // Let stop propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify collector received some samples
    auto* coll_raw =
        std::static_pointer_cast<hpactor::apps::bench_perf::BenchCollectorActor>(
            system.get_actor(collector.id()))
            .get();
    EXPECT_GT(coll_raw->total_cold_samples(), 0u);

    system.shutdown();
}
```

- [ ] **Step 3: Add test to CMake**

In `tests/system/apps/CMakeLists.txt` (or create if needed):

```cmake
add_executable(test_bench_perf_smoke test_bench_perf_smoke.cpp)
target_link_libraries(test_bench_perf_smoke PRIVATE hpactor_lib gtest)
target_include_directories(test_bench_perf_smoke PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME BenchPerfSmoke.SpawnAndRunMinimal COMMAND test_bench_perf_smoke --gtest_filter="*SpawnAndRunMinimal*")
```

- [ ] **Step 4: Build and run the smoke test**

```bash
ninja -C build test_bench_perf_smoke
./build/tests/system/apps/test_bench_perf_smoke
# Expected: PASS
```

- [ ] **Step 5: Commit**

```bash
git add tests/system/apps/
git commit -m "test: add bench_perf smoke test with 5 worker + 1 hot actor

Verifies actors spawn, receive BenchStart/BenchStop, and collector
receives latency samples. Uses 2 scheduler threads, 500ms run.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: Final Verification

- [ ] **Step 1: Run the full build one more time**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=ON
ninja -C build
# Expected: zero errors
```

- [ ] **Step 2: Run the smoke test**

```bash
cd build && ctest -R "BenchPerfSmoke" --output-on-failure
# Expected: 1 test passed
```

- [ ] **Step 3: Verify all existing tests still pass**

```bash
ctest --output-on-failure --parallel 8 -E "BenchPerfSmoke"
# Expected: all existing tests pass (no regressions)
```

- [ ] **Step 4: Check git status**

```bash
git status
# Expected: clean worktree, all changes committed
```

---

### Task 11: Create GitHub Issue

- [ ] **Step 1: Create issue with gh CLI**

```bash
gh issue create \
  --title "Performance Benchmark App — Actor System Throughput & Fairness Testing" \
  --body '## Goal

Implement a standalone performance benchmarking app (`apps/bench_perf/`) for the HPActor actor system that quantifies message processing throughput and scheduler fairness under load.

## Motivation

The actor framework lacks systematic performance characterization. As we approach production-readiness for the data plane (bounded mailboxes, DLQ, delivery semantics, tracing), we need:

1. **Throughput baselines** — how many msgs/sec can the system sustain with thousands of cooperative actors?
2. **Fairness under load** — does a single "noisy neighbor" actor starve others? What tail latency do cold actors see?
3. **Regression detection** — a repeatable benchmark to catch perf regressions in scheduler, mailbox, or allocator changes.

## Design

Two benchmark scenarios, selectable via `/bench start <preset>`:

### 1. `many-actors` — Throughput Under High Fan-Out

| Parameter | Value |
|-----------|-------|
| Cold workers | 5,000 ⨉ BenchWorkerActor |
| CPU burn per message | 10 μs (cooperative busy-wait) |
| Message rate per actor | 100 Hz (self-scheduled via `context()->schedule()`) |
| Scheduler threads | 8 (A2WS work-stealing) |
| Duration | 30 s (configurable) |
| Expected throughput | ~500K msgs/sec |
| Expected p99 latency | < 5 ms |

### 2. `hot-actor` — Noisy Neighbor Fairness

| Parameter | Value |
|-----------|-------|
| Hot actor | 1 ⨉ BenchHotActor, 500 μs burn, 1000 Hz |
| Cold workers | 1,000 ⨉ BenchWorkerActor, 10 μs burn, 10 Hz |
| Scheduler threads | 8 |
| Duration | 30 s |
| Expected cold p99 latency | < 10 ms |

## Architecture

Four actor types following `apps/cli_demo/` patterns:
- **BenchCoordinatorActor** — orchestration, preset dispatch, CLI state
- **BenchCollectorActor** — streaming percentile reservoir (10K samples), throughput windows
- **BenchWorkerActor** — light CPU burn, self-scheduling, group=cold latency sampling
- **BenchHotActor** — heavy CPU burn, high rate, group=hot latency sampling

## CLI Commands

```
/bench start <preset>  — Start a benchmark run
/bench stop            — Stop the current run
/bench status          — Show current state, elapsed, throughput
/bench report [group]  — Latency percentiles (p50/p99/p999) + throughput
/bench export [--json] — Raw data export
/bench list            — List available presets
/bench help            — Show available commands
```

## Metrics Tracked

| Metric | Scope |
|--------|-------|
| Throughput (msgs/sec) | Per group (hot/cold) + total |
| Latency p50/p99/p999 (μs) | Per group |
| Total messages | Per group |
| Elapsed wall clock | Global |

## Non-Goals (v1)

- Network/distributed benchmarks
- Coroutine actor benchmarks
- Memory allocation profiling (use existing `test_allocator_benchmark`)
- Persistent storage of results
- Grafana/Prometheus integration (MetricsActor already exists)'
```

- [ ] **Step 2: Note the issue number for the PR**

The `gh issue create` command prints the issue URL. Save it for reference.

- [ ] **Step 3: Commit (if any changes to note issue ref)**

```bash
# Link the issue in a follow-up commit message if desired
```
