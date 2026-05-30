# Fault Injection Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand fault injection coverage from 3 wired sites to ~117 across all 13 subsystems, with 14 independent fault domains, per-thread controller instances, probability expansion helper, and structured timeline logging.

**Architecture:** Builds on the existing `FaultController`/`FaultSchedule`/`FAULT_INJECT` framework from PR #153. Expands `FaultDomain` from 9 to 14 values, adds per-thread controller instances with a mutex-protected registry for broadcast operations, implements `expand_random()` on `FaultSchedule`, adds structured log emission on fault fire, and wires FAULT_INJECT macros into ~117 call sites across 4 implementation phases.

**Tech Stack:** C++20, header-only fault types + compiled fault runtime (hpactor_lib), Google Test

---

## Phase 1 — Infrastructure & Existing Unwired Points

### Task 1.1: Expand FaultDomain enum to 14 values

**Files:**
- Modify: `include/hpactor/fault/fault_types.hpp:22-32`

- [ ] **Step 1: Add 5 new domain enumerators**

```cpp
enum class FaultDomain : uint8_t {
    kMailbox = 0,
    kTransport = 1,
    kScheduler = 2,
    kAllocator = 3,
    kStorage = 4,
    kTimer = 5,
    kGossip = 6,
    kConfig = 7,
    kActor = 8,
    kRpc = 9,
    kSupervision = 10,
    kDiscovery = 11,
    kTracing = 12,
    kMetrics = 13,
};
```

- [ ] **Step 2: Update `to_string(FaultDomain)` to handle all 14 values**

```cpp
constexpr std::string_view to_string(FaultDomain d) noexcept {
    switch (d) {
    case FaultDomain::kMailbox:     return "kMailbox";
    case FaultDomain::kTransport:   return "kTransport";
    case FaultDomain::kScheduler:   return "kScheduler";
    case FaultDomain::kAllocator:   return "kAllocator";
    case FaultDomain::kStorage:     return "kStorage";
    case FaultDomain::kTimer:       return "kTimer";
    case FaultDomain::kGossip:      return "kGossip";
    case FaultDomain::kConfig:      return "kConfig";
    case FaultDomain::kActor:       return "kActor";
    case FaultDomain::kRpc:         return "kRpc";
    case FaultDomain::kSupervision: return "kSupervision";
    case FaultDomain::kDiscovery:   return "kDiscovery";
    case FaultDomain::kTracing:     return "kTracing";
    case FaultDomain::kMetrics:     return "kMetrics";
    }
    return "kUnknown";
}
```

- [ ] **Step 3: Build and verify no compile errors**

Run: `ninja -C build hpactor_lib`
Expected: Build succeeds (tests will fail until next tasks are done, but hpactor_lib should compile).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/fault/fault_types.hpp
git commit -m "feat(fault): expand FaultDomain from 9 to 14 values"
```

---

### Task 1.2: Expand domain_ticks_ arrays and assert checks

**Files:**
- Modify: `include/hpactor/fault/fault_controller.hpp:33,76` (Snapshot struct, domain_ticks_ member)
- Modify: `src/fault/fault_controller.cpp:58,73,115` (assert checks, snapshot loop)

- [ ] **Step 1: Update FaultControllerSnapshot to hold 14 domain ticks**

In `fault_controller.hpp:33`:
```cpp
struct FaultControllerSnapshot {
    bool enabled;
    std::string active_scope;
    uint64_t replay_seed;
    size_t schedule_entry_count;
    uint64_t domain_ticks[14];  // was [9]
    uint64_t faults_fired;
};
```

- [ ] **Step 2: Update FaultController private member**

In `fault_controller.hpp:76`:
```cpp
uint64_t domain_ticks_[14];  // was [9]
```

- [ ] **Step 3: Update assert checks in fault_controller.cpp**

In `fault_controller.cpp:58`, change `assert(idx < 9)` to `assert(idx < 14)`:
```cpp
void FaultController::advance_tick(FaultDomain domain) {
    auto idx = static_cast<size_t>(domain);
    assert(idx < 14);
    domain_ticks_[idx]++;
}
```

In `fault_controller.cpp:73`, same change:
```cpp
auto idx = static_cast<size_t>(domain);
assert(idx < 14);
```

- [ ] **Step 4: Update snapshot loop bound in fault_controller.cpp:115**

```cpp
for (size_t i = 0; i < 14; ++i) {  // was 9
    snap.domain_ticks[i] = domain_ticks_[i];
}
```

- [ ] **Step 5: Build and verify**

Run: `ninja -C build hpactor_lib`
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/fault/fault_controller.hpp src/fault/fault_controller.cpp
git commit -m "feat(fault): expand domain_ticks_ arrays and assert checks to 14 domains"
```

---

### Task 1.3: Add kFault log category

**Files:**
- Modify: `include/hpactor/log/detail/log_macros.hpp:39` (HPACTOR_LOG_CATEGORIES)

- [ ] **Step 1: Add kFault to the X-macro category list**

In `log_macros.hpp:39`, add after the `kUser` line:
```cpp
#define HPACTOR_LOG_CATEGORIES(X)                                              \
    X(kActor, "actor")                                                         \
    X(kActorState, "actor_state")                                              \
    X(kMailbox, "mailbox")                                                     \
    X(kScheduler, "scheduler")                                                 \
    X(kMemory, "memory")                                                       \
    X(kRegistrar, "registrar")                                                 \
    X(kDiscovery, "discovery")                                                 \
    X(kNetwork, "network")                                                     \
    X(kRpc, "rpc")                                                             \
    X(kConfig, "config")                                                       \
    X(kSupervision, "supervision")                                             \
    X(kCli, "cli")                                                             \
    X(kHttp, "http")                                                           \
    X(kFault, "fault")                                                         \
    X(kUser, "user")
```

- [ ] **Step 2: Build and verify the X-macro generates correctly**

Run: `ninja -C build hpactor_lib`
Expected: Build succeeds. The X-macro automatically generates the enum value, to_string case, and parse_category case.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/log/detail/log_macros.hpp
git commit -m "feat(log): add kFault log category for fault timeline"
```

---

### Task 1.4: Per-thread FaultController infrastructure

**Files:**
- Modify: `include/hpactor/fault/fault_controller.hpp:37-79` (class declaration)
- Modify: `src/fault/fault_controller.cpp:22-130` (implementation)

- [ ] **Step 1: Update header — add per-thread members and methods**

In `fault_controller.hpp`, replace the class declaration:

```cpp
class FaultController {
  public:
    FaultController();

    void load(const FaultSchedule& schedule);
    void clear();

    void enable(std::string_view scope_pattern);
    void disable(std::string_view scope_pattern);
    bool is_enabled() const noexcept { return enabled_; }

    bool check(std::string_view path,
               std::optional<ActorId> target = std::nullopt);

    void advance_tick(FaultDomain domain);

    void stall(FaultDomain domain, uint64_t delay_ticks);

    void set_replay_seed(uint64_t seed) { replay_seed_ = seed; }
    uint64_t replay_seed() const noexcept { return replay_seed_; }

    FaultControllerSnapshot snapshot() const;
    static FaultControllerSnapshot aggregate_snapshot();

    uint64_t faults_fired() const noexcept { return faults_fired_; }

    void install();
    void remove();

    static FaultController* instance();

  private:
    // Per-thread instance pointer (thread_local)
    static thread_local FaultController* tls_instance_;

    // Global registry of all per-thread instances
    struct InstanceList {
        std::mutex mutex;
        std::vector<FaultController*> instances;
    };
    static InstanceList& instance_list();

    void load_impl(const FaultSchedule& schedule);
    void clear_impl();
    void enable_impl(std::string_view scope_pattern);
    void disable_impl(std::string_view scope_pattern);

    bool enabled_;
    std::string active_scope_;
    FaultSchedule schedule_;
    size_t schedule_cursor_;
    uint64_t domain_ticks_[14];
    uint64_t replay_seed_;
    uint64_t faults_fired_;
};
```

- [ ] **Step 2: Update implementation — thread_local instance and registry**

In `fault_controller.cpp`, replace the implementation:

```cpp
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_point.hpp>
#include <hpactor/platform.hpp>

#include <cassert>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace hpactor::fault {

thread_local FaultController* FaultController::tls_instance_ = nullptr;

FaultController::InstanceList& FaultController::instance_list() {
    static InstanceList list;
    return list;
}

FaultController::FaultController()
    : enabled_(false)
    , active_scope_("*")
    , schedule_cursor_(0)
    , replay_seed_(0)
    , faults_fired_(0) {
    for (auto& tick : domain_ticks_) tick = 0;
}

void FaultController::load(const FaultSchedule& schedule) {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->load_impl(schedule);
    }
}

void FaultController::load_impl(const FaultSchedule& schedule) {
    schedule_ = schedule;
    schedule_cursor_ = 0;
    faults_fired_ = 0;
}

void FaultController::clear() {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->clear_impl();
    }
}

void FaultController::clear_impl() {
    schedule_.clear();
    schedule_cursor_ = 0;
    faults_fired_ = 0;
}

void FaultController::enable(std::string_view scope_pattern) {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->enable_impl(scope_pattern);
    }
}

void FaultController::enable_impl(std::string_view scope_pattern) {
    enabled_ = true;
    active_scope_ = std::string(scope_pattern);
}

void FaultController::disable(std::string_view scope_pattern) {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->disable_impl(scope_pattern);
    }
}

void FaultController::disable_impl(std::string_view /*scope_pattern*/) {
    enabled_ = false;
}

void FaultController::advance_tick(FaultDomain domain) {
    auto idx = static_cast<size_t>(domain);
    assert(idx < 14);
    domain_ticks_[idx]++;
}

bool FaultController::check(std::string_view path,
                             std::optional<ActorId> target) {
    if (HPACTOR_UNLIKELY(!enabled_)) return false;

    auto& registry = FaultPointRegistry::instance();
    if (!registry.matches_prefix(path, active_scope_)) return false;

    const auto* fault_point = registry.lookup(path);
    FaultDomain domain = fault_point ? fault_point->domain : FaultDomain::kMailbox;

    auto idx = static_cast<size_t>(domain);
    assert(idx < 14);
    domain_ticks_[idx]++;

    uint64_t current_tick = domain_ticks_[idx];

    for (size_t i = schedule_cursor_; i < schedule_.entries().size(); ++i) {
        const auto& entry = schedule_.entries()[i];
        if (entry.domain != domain) continue;
        if (entry.at_tick > current_tick) break;

        if (entry.at_tick == current_tick && entry.path == path) {
            if (entry.target.has_value() && target.has_value() &&
                entry.target.value() != target.value()) {
                continue;
            }
            schedule_cursor_ = i + 1;
            faults_fired_++;

            if (entry.action == FaultAction::kPanic) {
                std::abort();
            }

            return true;
        }
    }

    return false;
}

void FaultController::stall(FaultDomain domain, uint64_t delay_ticks) {
    auto idx = static_cast<size_t>(domain);
    for (uint64_t i = 0; i < delay_ticks; ++i) {
        domain_ticks_[idx]++;
    }
}

FaultControllerSnapshot FaultController::snapshot() const {
    FaultControllerSnapshot snap{};
    snap.enabled = enabled_;
    snap.active_scope = active_scope_;
    snap.replay_seed = replay_seed_;
    snap.schedule_entry_count = schedule_.size();
    for (size_t i = 0; i < 14; ++i) {
        snap.domain_ticks[i] = domain_ticks_[i];
    }
    snap.faults_fired = faults_fired_;
    return snap;
}

FaultControllerSnapshot FaultController::aggregate_snapshot() {
    FaultControllerSnapshot snap{};
    snap.enabled = false;
    snap.faults_fired = 0;
    for (size_t i = 0; i < 14; ++i) snap.domain_ticks[i] = 0;

    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        auto s = fc->snapshot();
        if (s.enabled) snap.enabled = true;
        snap.faults_fired += s.faults_fired;
        snap.schedule_entry_count = std::max(snap.schedule_entry_count, s.schedule_entry_count);
        for (size_t i = 0; i < 14; ++i) {
            snap.domain_ticks[i] += s.domain_ticks[i];
        }
        if (!s.active_scope.empty() && s.active_scope != "*") {
            snap.active_scope = s.active_scope;
        }
    }
    if (snap.active_scope.empty()) snap.active_scope = "*";

    return snap;
}

void FaultController::install() {
    tls_instance_ = this;
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    list.instances.push_back(this);
}

void FaultController::remove() {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    auto it = std::find(list.instances.begin(), list.instances.end(), this);
    if (it != list.instances.end()) {
        list.instances.erase(it);
    }
    tls_instance_ = nullptr;
}

FaultController* FaultController::instance() {
    return tls_instance_;
}

} // namespace hpactor::fault
```

- [ ] **Step 3: Build and verify**

Run: `ninja -C build hpactor_lib`
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/fault/fault_controller.hpp src/fault/fault_controller.cpp
git commit -m "feat(fault): per-thread FaultController instances with broadcast"
```

---

### Task 1.5: Implement expand_random() and FaultSchedule::sort()

**Files:**
- Modify: `include/hpactor/fault/fault_schedule.hpp:44-62` (FaultSchedule class)
- Modify: `src/fault/fault_schedule.cpp` (add expand_random + sort)

- [ ] **Step 1: Add sort() and expand_random() declarations to header**

In `fault_schedule.hpp`, add to `FaultSchedule` class:
```cpp
class FaultSchedule {
  public:
    // ... existing members ...

    void sort();  // sort entries by (domain, at_tick)

    template <typename RNG>
    FaultSchedule& expand_random(
        FaultDomain domain,
        std::string_view path,
        FaultAction action,
        double probability,
        uint64_t max_ticks,
        RNG& rng,
        FaultPayload payload = {},
        std::optional<ActorId> target = std::nullopt);

  private:
    std::vector<FaultScheduleEntry> entries_;
};
```

- [ ] **Step 2: Implement sort() in fault_schedule.cpp**

```cpp
#include <algorithm>

void FaultSchedule::sort() {
    std::sort(entries_.begin(), entries_.end(),
        [](const FaultScheduleEntry& a, const FaultScheduleEntry& b) {
            if (a.domain != b.domain)
                return static_cast<uint8_t>(a.domain) <
                       static_cast<uint8_t>(b.domain);
            return a.at_tick < b.at_tick;
        });
}
```

- [ ] **Step 3: Implement expand_random() in fault_schedule.cpp**

```cpp
#include <random>

template <typename RNG>
FaultSchedule& FaultSchedule::expand_random(
    FaultDomain domain,
    std::string_view path,
    FaultAction action,
    double probability,
    uint64_t max_ticks,
    RNG& rng,
    FaultPayload payload,
    std::optional<ActorId> target) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    for (uint64_t t = 0; t < max_ticks; ++t) {
        if (dist(rng) < probability) {
            FaultScheduleEntry entry{domain, t, std::string(path),
                                      action, target, payload};
            entries_.push_back(std::move(entry));
        }
    }
    return *this;
}
```

- [ ] **Step 4: Update FaultController::load() to sort schedule before broadcasting**

```cpp
void FaultController::load(const FaultSchedule& schedule) {
    // Sort a mutable copy before broadcasting
    FaultSchedule sorted = schedule;
    sorted.sort();
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->load_impl(sorted);
    }
}
```

Note: Need to add `#include <hpactor/fault/fault_schedule.hpp>` to fault_controller.cpp (already present implicitly).

- [ ] **Step 5: Build and verify**

Run: `ninja -C build hpactor_lib`
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/fault/fault_schedule.hpp src/fault/fault_schedule.cpp src/fault/fault_controller.cpp
git commit -m "feat(fault): implement expand_random() and schedule sort()"
```

---

### Task 1.6: Fault timeline log emission in check()

**Files:**
- Modify: `src/fault/fault_controller.cpp` (check() method)
- Modify: `src/actor/actor_system.cpp` (wire LogManager pointer)

- [ ] **Step 1: Add LogManager pointer and setter to FaultController header**

In `fault_controller.hpp`, add to the private section:
```cpp
  private:
    // ... existing members ...
    log::LogManager* log_manager_ = nullptr;
```

Add public setter:
```cpp
    void set_log_manager(log::LogManager* lm) { log_manager_ = lm; }
```

Add include:
```cpp
#include <hpactor/log/log_manager.hpp>
```

- [ ] **Step 2: Emit structured log on fault fire in check()**

In `fault_controller.cpp`, inside `check()`, after `faults_fired_++` and before `if (entry.action == FaultAction::kPanic)`:

```cpp
            schedule_cursor_ = i + 1;
            faults_fired_++;

            // Emit fault timeline log entry
            if (log_manager_) {
                log_manager_->emit(
                    log::LogLevel::kInfo,
                    log::LogCategory::kFault,
                    target.value_or(ActorId{0}),
                    0,
                    "fault_inject",
                    log::field("domain", to_string(entry.domain)),
                    log::field("tick", current_tick),
                    log::field("path", entry.path),
                    log::field("action", to_string(entry.action)),
                    log::field("schedule_index",
                               static_cast<uint64_t>(i)),
                    log::field("replay_seed", replay_seed_));
            }

            if (entry.action == FaultAction::kPanic) {
                std::abort();
            }
```

- [ ] **Step 3: Wire LogManager from ActorSystem**

In `actor_system.cpp`, find where `fault_controller_` is configured (near the end of the constructor or in a setup method). Add:

```cpp
    fault_controller_.set_log_manager(&log_manager_);
```

Note: This should be placed after `log_manager_` is initialized. Check `actor_system.cpp` for the appropriate location — likely near line 211 where `fault_controller_.install()` is called or in the constructor body.

- [ ] **Step 4: Build and verify**

Run: `ninja -C build hpactor_lib`
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/fault/fault_controller.hpp src/fault/fault_controller.cpp src/actor/actor_system.cpp
git commit -m "feat(fault): emit structured log entry on fault fire"
```

---

### Task 1.7: Wire 9 existing registered-but-unwired fault points

The 9 fault points already registered in `fault_points.cpp` but lacking FAULT_INJECT macros:
- `hpactor.mailbox.enqueue.fail` — ALREADY WIRED (line 174 in mpsc_actor_mailbox.hpp)
- `hpactor.mailbox.dequeue.drop` — ALREADY WIRED (line 238 in mpsc_actor_mailbox.hpp)
- `hpactor.transport.send.drop` — ALREADY WIRED (line 345 in tcp_transport.cpp)

Remaining to wire:
- `hpactor.transport.send.delay`
- `hpactor.transport.recv.drop`
- `hpactor.transport.recv.corrupt`
- `hpactor.transport.connection.reset`
- `hpactor.allocator.oom`
- `hpactor.actor.handler.delay`
- `hpactor.scheduler.worker.pause`
- `hpactor.scheduler.worker.panic`
- `hpactor.gossip.packet.loss`

This task wires them in sub-steps.

- [ ] **Step 1: Wire transport.send.delay in tcp_transport.cpp**

In `src/net/tcp_transport.cpp`, in `TcpTransport::try_send()`, add after the existing send.drop block (line 347):

```cpp
bool TcpTransport::try_send(const ActorAddress& target, const StreamBuffer& encoded) {
    FAULT_INJECT("hpactor.transport.send.drop") {
        return true;
    }
    FAULT_INJECT("hpactor.transport.send.delay") {
        _fc->stall(FaultDomain::kTransport, /*delay_ticks=*/3);
    }
    auto pool = get_or_create_pool(target.endpoint);
    return pool->try_send(target, encoded);
}
```

- [ ] **Step 2: Wire transport.recv.drop and transport.recv.corrupt in connection_pool.cpp**

Find `ConnectionPool::on_frame_received()` in `src/net/connection_pool.cpp` (around line 154). Add at entry:

```cpp
void ConnectionPool::on_frame_received(StreamBuffer buffer) {
    FAULT_INJECT("hpactor.transport.recv.drop") {
        return;  // silently drop the received frame
    }
    FAULT_INJECT("hpactor.transport.recv.corrupt") {
        if (buffer.size() > 0) {
            buffer.data()[0] ^= 0xFF;  // flip a byte
        }
        // fall through to normal processing with corrupted data
    }
    // ... existing frame dispatch logic ...
}
```

- [ ] **Step 3: Wire transport.connection.reset in tcp_transport.cpp**

In `TcpTransport::close_connection()` (around line 360):
```cpp
void TcpTransport::close_connection(EndPoint remote_endpoint) {
    FAULT_INJECT("hpactor.transport.connection.reset") {
        return;  // pretend to close but don't actually
    }
    auto it = pools_.find(remote_endpoint);
    // ... existing logic ...
}
```

- [ ] **Step 4: Wire allocator.oom in memory_config.cpp**

Find the `allocate()` free function in `src/mem/memory_config.cpp` (around line 49). Add at entry:

```cpp
void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept {
    FAULT_INJECT("hpactor.allocator.oom") {
        return nullptr;  // simulate OOM
    }
    // ... existing allocation logic ...
}
```

- [ ] **Step 5: Wire actor.handler.delay in event_based_actor.cpp**

Find `EventBasedActor::receive()` in `src/actor/event_based_actor.cpp` (around line 78). Add at entry:

```cpp
void EventBasedActor::receive(TypedMessage& msg) {
    FAULT_INJECT("hpactor.actor.handler.delay") {
        _fc->stall(FaultDomain::kActor, /*delay_ticks=*/5);
    }
    // ... existing receive logic ...
}
```

- [ ] **Step 6: Wire scheduler.worker.pause in scheduler.cpp**

Find `HybridScheduler::notify_ready()` in `src/sched/scheduler.cpp` (around line 115). Add at entry:

```cpp
void HybridScheduler::notify_ready(ActorId actor_id, uint8_t priority,
                                    int64_t deadline_ns) {
    FAULT_INJECT("hpactor.scheduler.worker.pause") {
        _fc->stall(FaultDomain::kScheduler, /*delay_ticks=*/5);
    }
    // ... existing notify_ready logic ...
}
```

- [ ] **Step 7: Wire scheduler.worker.panic in scheduler.cpp**

Find `HybridScheduler::worker_loop()` in `src/sched/scheduler.cpp` (around line 508). Add at entry:

```cpp
void HybridScheduler::worker_loop(uint32_t worker_index) {
    FAULT_INJECT("hpactor.scheduler.worker.panic") {
        // unreachable — FaultController::check() calls std::abort() for kPanic
    }
    // ... existing worker_loop logic ...
}
```

- [ ] **Step 8: Wire gossip.packet.loss in gossip_membership.cpp**

Find the central UDP send method in `src/net/gossip_membership.cpp`. Look for `async_udp_send()` or the generic send method (around line 148). Add at entry:

```cpp
// In the generic outbound gossip send path
FAULT_INJECT("hpactor.gossip.packet.loss") {
    return;  // silently drop the packet
}
```

- [ ] **Step 9: Add required #include for fault_macros.hpp in each modified source**

Ensure each modified .cpp file includes `<hpactor/fault/fault_macros.hpp>`. Check:
- `src/mem/memory_config.cpp`
- `src/actor/event_based_actor.cpp`
- `src/sched/scheduler.cpp`
- `src/net/gossip_membership.cpp`

- [ ] **Step 10: Build and verify**

Run: `ninja -C build`
Expected: Build succeeds with all 12 fault points now wired.

- [ ] **Step 11: Run existing fault tests to verify no regressions**

Run: `ctest -R "fault" --output-on-failure`
Expected: All existing fault tests pass.

- [ ] **Step 12: Commit**

```bash
git add src/net/tcp_transport.cpp src/net/connection_pool.cpp src/mem/memory_config.cpp src/actor/event_based_actor.cpp src/sched/scheduler.cpp src/net/gossip_membership.cpp
git commit -m "feat(fault): wire 9 existing registered-but-unwired fault points"
```

---

### Task 1.8: Update unit tests for Phase 1 infrastructure

**Files:**
- Modify: `tests/unit/fault/test_fault_controller.cpp` (expand with per-thread tests)
- Modify: `tests/unit/fault/test_fault_schedule.cpp` (expand with expand_random tests)
- Create: `tests/unit/fault/test_fault_macro.cpp` (new — macro expansion patterns)

- [ ] **Step 1: Add per-thread controller tests**

In `tests/unit/fault/test_fault_controller.cpp`, add:

```cpp
TEST(FaultController, PerThreadInstall) {
    FaultController fc1;
    FaultController fc2;

    fc1.install();
    EXPECT_EQ(FaultController::instance(), &fc1);

    fc2.install();  // fc2 replaces fc1 on THIS thread
    EXPECT_EQ(FaultController::instance(), &fc2);

    fc2.remove();
    EXPECT_EQ(FaultController::instance(), nullptr);
}

TEST(FaultController, AggregateSnapshotSumsAcrossInstances) {
    FaultController fc1;
    FaultController fc2;

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kMailbox, 0)
            .fail("hpactor.mailbox.enqueue.fail", 1));

    // fc1 becomes master, loads schedule (broadcasts to all)
    fc1.load(schedule);
    fc1.enable("*");
    fc1.install();

    fc2.load(schedule);
    fc2.enable("*");
    fc2.install();

    // Simulate a fault fire on fc1 only
    fc1.advance_tick(FaultDomain::kMailbox);
    fc1.check("hpactor.mailbox.enqueue.fail");

    auto snap = FaultController::aggregate_snapshot();
    EXPECT_EQ(snap.faults_fired, 1);

    fc1.remove();
    fc2.remove();
}

TEST(FaultController, BroadcastLoad) {
    FaultController fc1;
    FaultController fc2;

    fc1.install();
    fc2.install();

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kMailbox, 0)
            .fail("hpactor.mailbox.enqueue.fail", 1));

    fc1.load(schedule);  // broadcasts to all installed instances
    fc1.enable("*");     // broadcasts

    EXPECT_TRUE(fc1.is_enabled());
    EXPECT_TRUE(fc2.is_enabled());
    EXPECT_EQ(fc1.snapshot().schedule_entry_count, 1);
    EXPECT_EQ(fc2.snapshot().schedule_entry_count, 1);

    fc1.remove();
    fc2.remove();
}

TEST(FaultController, RemoveCleansUp) {
    FaultController fc;
    fc.install();
    EXPECT_EQ(FaultController::instance(), &fc);

    fc.remove();
    EXPECT_EQ(FaultController::instance(), nullptr);

    // aggregate snapshot should handle empty list
    auto snap = FaultController::aggregate_snapshot();
    EXPECT_EQ(snap.faults_fired, 0);
}
```

- [ ] **Step 2: Add expand_random() tests**

In `tests/unit/fault/test_fault_schedule.cpp`, add:

```cpp
TEST(FaultSchedule, ExpandRandomGeneratesEntriesAtProbability) {
    FaultSchedule schedule;
    std::mt19937 rng(42);

    schedule.expand_random(FaultDomain::kTransport,
                           "hpactor.transport.send.drop",
                           FaultAction::kDrop,
                           /*probability=*/0.1,
                           /*max_ticks=*/1000,
                           rng);

    // With probability 0.1 over 1000 ticks, expect ~100 entries
    // With seed 42, the count should be deterministic
    size_t count = schedule.size();
    EXPECT_GT(count, 50);
    EXPECT_LT(count, 150);

    // All entries should have the correct domain and action
    for (const auto& entry : schedule.entries()) {
        EXPECT_EQ(entry.domain, FaultDomain::kTransport);
        EXPECT_EQ(entry.path, "hpactor.transport.send.drop");
        EXPECT_EQ(entry.action, FaultAction::kDrop);
        EXPECT_LT(entry.at_tick, 1000);
    }
}

TEST(FaultSchedule, ExpandRandomDeterministic) {
    FaultSchedule s1, s2;
    std::mt19937 rng1(12345);
    std::mt19937 rng2(12345);  // same seed

    s1.expand_random(FaultDomain::kTransport, "hpactor.transport.send.drop",
                     FaultAction::kDrop, 0.05, 500, rng1);
    s2.expand_random(FaultDomain::kTransport, "hpactor.transport.send.drop",
                     FaultAction::kDrop, 0.05, 500, rng2);

    EXPECT_EQ(s1.size(), s2.size());
    for (size_t i = 0; i < s1.size(); ++i) {
        EXPECT_EQ(s1.entries()[i].at_tick, s2.entries()[i].at_tick);
    }
}

TEST(FaultSchedule, SortOrdersByDomainThenTick) {
    FaultSchedule schedule;

    schedule.add_entry(FaultScheduleEntry{
        FaultDomain::kTransport, 5, "b", FaultAction::kDrop, {}, {}});
    schedule.add_entry(FaultScheduleEntry{
        FaultDomain::kMailbox, 10, "a", FaultAction::kFail, {}, {}});
    schedule.add_entry(FaultScheduleEntry{
        FaultDomain::kMailbox, 3, "c", FaultAction::kDrop, {}, {}});

    schedule.sort();

    const auto& entries = schedule.entries();
    EXPECT_EQ(entries[0].domain, FaultDomain::kMailbox);
    EXPECT_EQ(entries[0].at_tick, 3);
    EXPECT_EQ(entries[1].domain, FaultDomain::kMailbox);
    EXPECT_EQ(entries[1].at_tick, 10);
    EXPECT_EQ(entries[2].domain, FaultDomain::kTransport);
    EXPECT_EQ(entries[2].at_tick, 5);
}
```

- [ ] **Step 3: Create test_fault_macro.cpp**

Create `tests/unit/fault/test_fault_macro.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/fault/fault_schedule.hpp>

#include <gtest/gtest.h>

namespace hpactor::fault {
namespace {

TEST(FaultMacro, MacroNoOpWhenDisabled) {
    FaultController fc;
    fc.install();

    // Without loading a schedule or enabling, check() returns false
    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_FALSE(fault_fired);

    fc.remove();
}

TEST(FaultMacro, MacroNoOpWhenNotInstalled) {
    // No install() call — tls_instance_ is nullptr
    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_FALSE(fault_fired);
}

TEST(FaultMacro, MacroFiresWhenScheduled) {
    FaultController fc;
    fc.install();

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kMailbox, 0)
            .fail("hpactor.mailbox.enqueue.fail", 1));

    fc.load(schedule);
    fc.enable("*");

    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_TRUE(fault_fired);
    EXPECT_EQ(fc.faults_fired(), 1);

    fc.remove();
}

TEST(FaultMacro, MacroDoesNotFireForDifferentPath) {
    FaultController fc;
    fc.install();

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kTransport, 0)
            .drop("hpactor.transport.send.drop"));

    fc.load(schedule);
    fc.enable("*");

    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {  // different path
        fault_fired = true;
    }
    EXPECT_FALSE(fault_fired);

    fc.remove();
}

TEST(FaultMacro, MacroOnlyFiresAtCorrectTick) {
    FaultController fc;
    fc.install();

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kMailbox, 5)
            .fail("hpactor.mailbox.enqueue.fail", 1));

    fc.load(schedule);
    fc.enable("*");

    // Ticks 0-4 should not fire
    for (int i = 0; i < 5; i++) {
        bool fault_fired = false;
        FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
            fault_fired = true;
        }
        EXPECT_FALSE(fault_fired) << "at tick " << i;
    }

    // Tick 5 should fire
    bool fault_fired = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        fault_fired = true;
    }
    EXPECT_TRUE(fault_fired);

    fc.remove();
}

} // anonymous namespace
} // namespace hpactor::fault
```

- [ ] **Step 4: Add new test file to CMakeLists.txt**

In `tests/unit/fault/CMakeLists.txt`, add `test_fault_macro.cpp`:
```cmake
add_library(test_fault_macro OBJECT test_fault_macro.cpp)
target_link_libraries(test_fault_macro PUBLIC hpactor_lib GTest::gtest)
```

Add to the existing test binary or create a new one. Check current structure:

```bash
grep -r "test_fault" tests/unit/fault/CMakeLists.txt
```

- [ ] **Step 5: Build and run tests**

Run: `ninja -C build`
Run: `ctest -R "fault" --output-on-failure`
Expected: All fault tests pass.

- [ ] **Step 6: Commit**

```bash
git add tests/unit/fault/test_fault_controller.cpp tests/unit/fault/test_fault_schedule.cpp tests/unit/fault/test_fault_macro.cpp tests/unit/fault/CMakeLists.txt
git commit -m "test(fault): add per-thread, expand_random, and macro tests"
```

---

### Task 1.9: Register new-domain fault points in fault_points.cpp

**Files:**
- Modify: `src/fault/fault_points.cpp` (add registrations for 5 new domains)

- [ ] **Step 1: Add representative registrations for new domains**

Add to the anonymous namespace in `fault_points.cpp`:

```cpp
// --- New domain registrations (Phase 1 — infrastructure) ---

// kRpc
const FaultPointRegistrar kRpcSendDrop{
    "hpactor.rpc.send.drop", FaultDomain::kRpc,
    "RPC request silently dropped"};
const FaultPointRegistrar kRpcResponseDrop{
    "hpactor.rpc.response.drop", FaultDomain::kRpc,
    "RPC response silently dropped"};

// kSupervision
const FaultPointRegistrar kSupervisionRestartDrop{
    "hpactor.supervision.restart_child.drop", FaultDomain::kSupervision,
    "Supervision child restart silently skipped"};
const FaultPointRegistrar kSupervisionHandleDownDrop{
    "hpactor.supervision.handle_child_down.drop", FaultDomain::kSupervision,
    "Supervision child death notification dropped"};

// kDiscovery
const FaultPointRegistrar kDiscoveryHeartbeatDrop{
    "hpactor.discovery.heartbeat.drop", FaultDomain::kDiscovery,
    "Discovery heartbeat silently dropped"};
const FaultPointRegistrar kDiscoveryRegisterDrop{
    "hpactor.discovery.register.drop", FaultDomain::kDiscovery,
    "Discovery registration silently dropped"};

// kTracing
const FaultPointRegistrar kTracingStartSpanDrop{
    "hpactor.tracing.start_span.drop", FaultDomain::kTracing,
    "Tracing span start silently dropped"};
const FaultPointRegistrar kTracingExporterFail{
    "hpactor.tracing.exporter.export.fail", FaultDomain::kTracing,
    "Tracing exporter backend failure"};

// kMetrics
const FaultPointRegistrar kMetricsRingBufferPushFail{
    "hpactor.metrics.ring_buffer.push.fail", FaultDomain::kMetrics,
    "Metrics ring buffer push failure"};
const FaultPointRegistrar kMetricsFormatterCorrupt{
    "hpactor.metrics.formatter.format.corrupt", FaultDomain::kMetrics,
    "Metrics formatter output corruption"};
```

- [ ] **Step 2: Build and verify**

Run: `ninja -C build hpactor_lib`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/fault/fault_points.cpp
git commit -m "feat(fault): register representative fault points for 5 new domains"
```

---

## Phase 2 — Tier 1: Core Message/Resource Path

### Task 2.1: Expand mailbox fault points (8 sites)

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` (try_push, push, enqueue_reserved, drain_overflow, drop_one_oldest)
- Modify: `include/hpactor/mailbox/dead_letter_queue.hpp` (try_push)
- Modify: `include/hpactor/mailbox/dedup_cache.hpp` (is_duplicate)
- Modify: `include/hpactor/mailbox/overflow_queue.hpp` (try_push)

- [ ] **Step 1: Wire try_push.fail (already conceptually done via enqueue.fail — verify)**

Check `enqueue()` at line 174 already has `hpactor.mailbox.enqueue.fail`. The `try_push` path should also have a fault point at the reservation stage. If `try_push` delegates to a reservation manager, place FAULT_INJECT there.

Locate `try_push` in `mpsc_actor_mailbox.hpp`. The current code around line 155-166 shows `try_push` calling `try_reserve` which returns a `ReservationResult`. Add a fault injection there:

```cpp
EnqueueResult try_push(T&& msg, MailboxEnvelopeMeta meta = {}) noexcept {
    FAULT_INJECT("hpactor.mailbox.try_push.fail") {
        return EnqueueResult::failure(FailureReason::kMailboxFull);
    }
    // ... existing try_push logic ...
}
```

- [ ] **Step 2: Add new registrations in fault_points.cpp**

Add the expanded mailbox points to `fault_points.cpp`:

```cpp
const FaultPointRegistrar kMailboxTryPushFail{
    "hpactor.mailbox.try_push.fail", FaultDomain::kMailbox,
    "Mailbox admission gate returns failure"};
const FaultPointRegistrar kMailboxEnqueueReservedDrop{
    "hpactor.mailbox.enqueue_reserved.drop", FaultDomain::kMailbox,
    "Silent drop after capacity committed"};
const FaultPointRegistrar kMailboxDrainOverflowFail{
    "hpactor.mailbox.drain_overflow.fail", FaultDomain::kMailbox,
    "Overflow drain stall"};
const FaultPointRegistrar kMailboxDropOldestFail{
    "hpactor.mailbox.drop_oldest.fail", FaultDomain::kMailbox,
    "DropHead eviction failure"};
const FaultPointRegistrar kMailboxDlqPushDrop{
    "hpactor.mailbox.dlq.push.drop", FaultDomain::kMailbox,
    "Dead-letter record silently dropped"};
const FaultPointRegistrar kMailboxDedupCorrupt{
    "hpactor.mailbox.dedup.is_duplicate.corrupt", FaultDomain::kMailbox,
    "Duplicate detection returns wrong answer"};
const FaultPointRegistrar kMailboxOverflowPushDrop{
    "hpactor.mailbox.overflow.push.drop", FaultDomain::kMailbox,
    "Overflow queue push silently dropped"};
```

- [ ] **Step 3: Wire remaining mailbox sites**

In `enqueue_reserved` method (around line 197):
```cpp
void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta,
                      bool suppress_wakeup = false) noexcept {
    FAULT_INJECT("hpactor.mailbox.enqueue_reserved.drop") {
        return;  // silently drop: capacity was committed but message lost
    }
    // ... existing logic ...
}
```

In `drain_overflow` (around line 392):
```cpp
void drain_overflow() noexcept {
    FAULT_INJECT("hpactor.mailbox.drain_overflow.fail") {
        return;  // pretend we drained but didn't
    }
    // ... existing logic ...
}
```

In `drop_one_oldest` (around line 353):
```cpp
bool drop_one_oldest() noexcept {
    FAULT_INJECT("hpactor.mailbox.drop_oldest.fail") {
        return false;  // eviction failed
    }
    // ... existing logic ...
}
```

In `DedupCache::is_duplicate`:
```cpp
bool is_duplicate(const DedupKey& key) {
    bool result = lookup_impl(key);  // existing implementation
    FAULT_INJECT("hpactor.mailbox.dedup.is_duplicate.corrupt") {
        result = !result;  // flip the answer
    }
    return result;
}
```

In DLQ `try_push`:
```cpp
bool try_push(DeadLetterRecord&& record) {
    FAULT_INJECT("hpactor.mailbox.dlq.push.drop") {
        return false;  // record silently lost
    }
    // ... existing logic ...
}
```

In overflow queue `try_push`:
```cpp
bool try_push(T&& msg) {
    FAULT_INJECT("hpactor.mailbox.overflow.push.drop") {
        return false;  // silently fail to push
    }
    // ... existing logic ...
}
```

- [ ] **Step 4: Build and run tests**

Run: `ninja -C build`
Run: `ctest -R "fault" --output-on-failure`
Expected: Build succeeds, existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp include/hpactor/mailbox/dead_letter_queue.hpp include/hpactor/mailbox/dedup_cache.hpp include/hpactor/mailbox/overflow_queue.hpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand mailbox fault points to 10 wired sites"
```

---

### Task 2.2: Expand transport fault points (15 sites)

**Files:**
- Modify: `src/net/tcp_transport.cpp` (send.delay already wired, add send.corrupt)
- Modify: `src/net/connection_pool.cpp` (send.drop/delay, try_send.fail, reconnect, flush, pending, frame)
- Modify: `src/net/wireframe_connection.cpp` (handle_read, flush_write_buffer)
- Modify: `src/net/acceptor.cpp` (listen, accept)
- Modify: `src/fault/fault_points.cpp` (registrations)

- [ ] **Step 1: Add transport fault point registrations**

```cpp
const FaultPointRegistrar kTransportSendCorrupt{
    "hpactor.transport.send.corrupt", FaultDomain::kTransport,
    "Transport send data corruption"};
const FaultPointRegistrar kPoolSendDrop{
    "hpactor.connection_pool.send.drop", FaultDomain::kTransport,
    "Connection pool send silently dropped"};
const FaultPointRegistrar kPoolReconnectDrop{
    "hpactor.connection_pool.reconnect.drop", FaultDomain::kTransport,
    "Connection pool reconnect prevented"};
const FaultPointRegistrar kPoolFlushDrop{
    "hpactor.connection_pool.flush.drop", FaultDomain::kTransport,
    "Connection pool flush silently drops messages"};
const FaultPointRegistrar kPoolFrameDrop{
    "hpactor.connection_pool.frame.drop", FaultDomain::kTransport,
    "Connection pool received frame dropped"};
const FaultPointRegistrar kWireframeReadDrop{
    "hpactor.wireframe.handle_read.drop", FaultDomain::kTransport,
    "Wireframe read silently dropped"};
const FaultPointRegistrar kWireframeWriteDrop{
    "hpactor.wireframe.flush_write_buffer.drop", FaultDomain::kTransport,
    "Wireframe write buffer never flushed"};
const FaultPointRegistrar kAcceptorListenFail{
    "hpactor.acceptor.listen.fail", FaultDomain::kTransport,
    "Acceptor bind/listen failure"};
const FaultPointRegistrar kAcceptorAcceptDrop{
    "hpactor.acceptor.accept.drop", FaultDomain::kTransport,
    "Accepted connection silently dropped"};
```

- [ ] **Step 2: Wire tcp_transport send.corrupt**

In `tcp_transport.cpp`, in `try_send`:
```cpp
FAULT_INJECT("hpactor.transport.send.corrupt") {
    // Corrupt the encoded buffer before sending
    auto& mutable_buffer = const_cast<StreamBuffer&>(encoded);
    if (mutable_buffer.size() > 0) {
        mutable_buffer.data()[0] ^= 0xFF;
    }
    // fall through to normal send with corrupted data
}
```

- [ ] **Step 3: Wire connection_pool sites**

In `connection_pool.cpp`:

`send()` method:
```cpp
bool ConnectionPool::send(const ActorAddress& target, const StreamBuffer& encoded) {
    FAULT_INJECT("hpactor.connection_pool.send.drop") {
        return true;  // claim success
    }
    // ... existing logic ...
}
```

`try_send()` method:
```cpp
bool ConnectionPool::try_send(const ActorAddress& target, const StreamBuffer& encoded) {
    FAULT_INJECT("hpactor.connection_pool.try_send.fail") {
        return false;  // admission denied
    }
    // ... existing logic ...
}
```

`schedule_reconnect()` method:
```cpp
void ConnectionPool::schedule_reconnect() {
    FAULT_INJECT("hpactor.connection_pool.reconnect.drop") {
        return;  // never reconnect
    }
    // ... existing logic ...
}
```

`flush_pending()` method:
```cpp
void ConnectionPool::flush_pending() {
    FAULT_INJECT("hpactor.connection_pool.flush.drop") {
        pending_.clear();  // clear without sending
        return;
    }
    // ... existing logic ...
}
```

`on_frame_received()` method — already has recv.drop and recv.corrupt from Task 1.7. Add frame.drop:
```cpp
FAULT_INJECT("hpactor.connection_pool.frame.drop") {
    return;  // drop received frame
}
```

- [ ] **Step 4: Wire wireframe sites**

In `wireframe_connection.cpp`:

`handle_read()` entry:
```cpp
void WireFrameConnection::handle_read() {
    FAULT_INJECT("hpactor.wireframe.handle_read.drop") {
        // drain the read buffer without processing
        char discard[4096];
        while (read(discard, sizeof(discard)) > 0) {}
        return;
    }
    // ... existing logic ...
}
```

`flush_write_buffer()`:
```cpp
void WireFrameConnection::flush_write_buffer() {
    FAULT_INJECT("hpactor.wireframe.flush_write_buffer.drop") {
        write_buffer_.clear();
        return;  // pretend we flushed
    }
    // ... existing logic ...
}
```

- [ ] **Step 5: Wire acceptor sites**

In `acceptor.cpp`:

`listen()`:
```cpp
bool TcpAcceptor::listen(uint16_t port, uint16_t backlog, const std::string& bind_addr) {
    FAULT_INJECT("hpactor.acceptor.listen.fail") {
        return false;  // bind/listen failure
    }
    // ... existing logic ...
}
```

`handle_read()` (the accept loop):
```cpp
void TcpAcceptor::handle_read() {
    FAULT_INJECT("hpactor.acceptor.accept.drop") {
        // accept the connection but don't deliver it
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd >= 0) ::close(fd);
        return;
    }
    // ... existing accept loop ...
}
```

- [ ] **Step 6: Build and run tests**

Run: `ninja -C build`
Run: `ctest -R "fault" --output-on-failure`
Expected: Build succeeds, existing tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/net/tcp_transport.cpp src/net/connection_pool.cpp src/net/wireframe_connection.cpp src/net/acceptor.cpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand transport fault points to 15 wired sites"
```

---

### Task 2.3: Expand scheduler fault points (12 sites)

**Files:**
- Modify: `src/sched/scheduler.cpp` (notify_ready, try_steal, pop_local, execute_actor, worker_loop, process_actor, reenqueue)
- Modify: `src/sched/timing_wheel.cpp` (schedule, advance, cancel)
- Modify: `src/fault/fault_points.cpp` (registrations)

Note: The existing `hpactor.scheduler.worker.pause` and `hpactor.scheduler.worker.panic` are already wired from Task 1.7. This task adds the remaining 10 scheduler + 3 timer points.

- [ ] **Step 1: Add scheduler/timer fault point registrations**

```cpp
// Scheduler
const FaultPointRegistrar kSchedulerNotifyReadyDrop{
    "hpactor.scheduler.notify_ready.drop", FaultDomain::kScheduler,
    "Scheduler notify_ready silently dropped (lost wakeup)"};
const FaultPointRegistrar kSchedulerTryStealFail{
    "hpactor.scheduler.try_steal.fail", FaultDomain::kScheduler,
    "Scheduler try_steal returns false when work exists"};
const FaultPointRegistrar kSchedulerPopLocalFail{
    "hpactor.scheduler.pop_local.fail", FaultDomain::kScheduler,
    "Scheduler pop_local returns false for non-empty queue"};
const FaultPointRegistrar kSchedulerExecuteMsgDrop{
    "hpactor.scheduler.execute_actor.msg_drop", FaultDomain::kScheduler,
    "Scheduler drops message mid-execution"};
const FaultPointRegistrar kSchedulerExecuteDispatchSkip{
    "hpactor.scheduler.execute_actor.dispatch_skip", FaultDomain::kScheduler,
    "Scheduler dequeues actor but skips dispatch"};
const FaultPointRegistrar kSchedulerWorkerExitEarly{
    "hpactor.scheduler.worker_loop.exit_early", FaultDomain::kScheduler,
    "Scheduler worker thread exits prematurely"};
const FaultPointRegistrar kSchedulerProcessActorDelay{
    "hpactor.scheduler.process_actor.delay", FaultDomain::kScheduler,
    "Scheduler process_actor stalls"};
const FaultPointRegistrar kSchedulerReenqueueDrop{
    "hpactor.scheduler.reenqueue_drop", FaultDomain::kScheduler,
    "Scheduler fails to re-enqueue actor after processing"};

// Timer (kTimer domain)
const FaultPointRegistrar kTimerScheduleFail{
    "hpactor.timing_wheel.schedule.fail", FaultDomain::kTimer,
    "Timing wheel schedule silently dropped"};
const FaultPointRegistrar kTimerAdvanceSkip{
    "hpactor.timing_wheel.advance.skip", FaultDomain::kTimer,
    "Timing wheel advance skips expired timers"};
const FaultPointRegistrar kTimerCancelFail{
    "hpactor.timing_wheel.cancel.fail", FaultDomain::kTimer,
    "Timing wheel cancel silently ignored"};
```

- [ ] **Step 2: Wire scheduler.cpp sites**

In `scheduler.cpp`:

`notify_ready()`:
```cpp
void HybridScheduler::notify_ready(ActorId actor_id, uint8_t priority,
                                    int64_t deadline_ns) {
    FAULT_INJECT("hpactor.scheduler.notify_ready.drop") {
        return;  // actor never enqueued — lost wakeup
    }
    // ... existing logic (including worker.pause from Task 1.7) ...
}
```

`try_steal()` (around line 207):
```cpp
bool HybridScheduler::try_steal(WorkerThread& thief, WorkItem& out) {
    FAULT_INJECT("hpactor.scheduler.try_steal.fail") {
        return false;  // steal blindness
    }
    // ... existing logic ...
}
```

`pop_local()` (around line 263):
```cpp
bool HybridScheduler::pop_local(WorkerThread& worker, WorkItem& out) {
    FAULT_INJECT("hpactor.scheduler.pop_local.fail") {
        return false;  // spurious empty
    }
    // ... existing logic ...
}
```

`execute_actor()` (around line 336):
```cpp
void HybridScheduler::execute_actor(WorkerThread& worker, const WorkItem& item) {
    FAULT_INJECT("hpactor.scheduler.execute_actor.dispatch_skip") {
        return;  // actor dequeued but never executed
    }
    // ... existing logic ...
}
```

In the message processing loop inside `execute_actor()` or `process_actor()`, where messages are popped from the mailbox (around line 440):
```cpp
// After mailbox->try_pop(msg) succeeds:
FAULT_INJECT("hpactor.scheduler.execute_actor.msg_drop") {
    continue;  // or break — skip this message
}
```

`worker_loop()` (around line 508 — already has worker.panic from Task 1.7):
```cpp
void HybridScheduler::worker_loop(uint32_t worker_index) {
    FAULT_INJECT("hpactor.scheduler.worker_loop.exit_early") {
        return;  // thread exits the loop prematurely
    }
    // ... existing logic ...
}
```

`process_actor()` (around line 299):
```cpp
void HybridScheduler::process_actor(WorkerThread& worker, ActorId actor_id) {
    FAULT_INJECT("hpactor.scheduler.process_actor.delay") {
        _fc->stall(FaultDomain::kScheduler, /*delay_ticks=*/3);
    }
    // ... existing logic ...
}
```

Re-enqueue path (around line 460-469, where actor is re-enqueued after processing):
```cpp
// In the block that re-enqueues:
FAULT_INJECT("hpactor.scheduler.reenqueue_drop") {
    return;  // actor not re-enqueued, starvation
}
```

- [ ] **Step 3: Wire timing_wheel.cpp sites**

In `timing_wheel.hpp` or the implementation:

`schedule()`:
```cpp
uint64_t TimingWheel::schedule(int64_t delay_ns, TimerCallback cb) {
    FAULT_INJECT("hpactor.timing_wheel.schedule.fail") {
        return 0;  // invalid handle, timer never registered
    }
    // ... existing logic ...
}
```

`advance()`:
```cpp
uint32_t TimingWheel::advance(int64_t now_ns) {
    FAULT_INJECT("hpactor.timing_wheel.advance.skip") {
        return 0;  // no timers fired
    }
    // ... existing logic ...
}
```

`cancel()`:
```cpp
bool TimingWheel::cancel(uint64_t timer_id) {
    FAULT_INJECT("hpactor.timing_wheel.cancel.fail") {
        return false;  // cancel fails silently
    }
    // ... existing logic ...
}
```

- [ ] **Step 4: Build and run tests**

Run: `ninja -C build`
Run: `ctest -R "fault" --output-on-failure`
Expected: Build succeeds, existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/sched/scheduler.cpp src/sched/timing_wheel.cpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand scheduler and timer fault points to 12 sites"
```

---

### Task 2.4: Expand allocator fault points (8 sites)

**Files:**
- Modify: `src/mem/memory_config.cpp` (allocate — already has oom from Task 1.7, add deallocate fault)
- Modify: `src/mem/segment_provider.cpp` (acquire_slab, allocate_new_segment)
- Modify: `src/mem/slab_cache.cpp` (allocate, refill)
- Modify: `src/mem/memory_region.cpp` (try_reserve, record_free)
- Modify: `src/fault/fault_points.cpp` (registrations)

- [ ] **Step 1: Add allocator fault point registrations**

```cpp
const FaultPointRegistrar kAllocatorSegmentMmapFail{
    "hpactor.allocator.segment.mmap_fail", FaultDomain::kAllocator,
    "SegmentProvider mmap returns MAP_FAILED"};
const FaultPointRegistrar kAllocatorFreelistPopCorrupt{
    "hpactor.allocator.freelist.pop.corrupt", FaultDomain::kAllocator,
    "Freelist pop returns corrupted node"};
const FaultPointRegistrar kAllocatorSlabRefillFail{
    "hpactor.allocator.slab_cache.refill_fail", FaultDomain::kAllocator,
    "SlabCache refill returns nullptr"};
const FaultPointRegistrar kAllocatorRegionTryReserveFail{
    "hpactor.allocator.region.try_reserve.fail", FaultDomain::kAllocator,
    "Memory region hard-limit rejection"};
const FaultPointRegistrar kAllocatorRegionRecordFreeSkip{
    "hpactor.allocator.region.record_free.skip", FaultDomain::kAllocator,
    "Memory region record_free silently skipped"};
const FaultPointRegistrar kAllocatorCanaryCorrupt{
    "hpactor.allocator.canary.verify.corrupt", FaultDomain::kAllocator,
    "CanaryFooter verify returns wrong answer"};
```

- [ ] **Step 2: Wire segment_provider.cpp sites**

In `SegmentProvider::allocate_new_segment()` (around line 165):
```cpp
void* SegmentProvider::allocate_new_segment(size_t size) {
    FAULT_INJECT("hpactor.allocator.segment.mmap_fail") {
        return nullptr;  // MAP_FAILED — root OOM
    }
    // ... existing mmap logic ...
}
```

- [ ] **Step 3: Wire slab_cache.cpp sites**

In `SlabCache::refill()` (around line 124):
```cpp
void SlabCache::refill() {
    FAULT_INJECT("hpactor.allocator.slab_cache.refill_fail") {
        return;  // leave current_slab_ as nullptr
    }
    // ... existing refill logic ...
}
```

In `SlabCache::allocate()` where freelist pop happens (around line 38):
```cpp
auto* block = freelist_.pop();
FAULT_INJECT("hpactor.allocator.freelist.pop.corrupt") {
    // Return a block with corrupted header
    if (block) {
        block->magic = ~AllocHeader::kAllocMagic;  // corrupt the magic
    }
    // fall through with corrupted block
}
```

- [ ] **Step 4: Wire memory_region.cpp sites**

In `try_reserve()` (around line 45):
```cpp
bool try_reserve(RegionType region, size_t charged_bytes) noexcept {
    FAULT_INJECT("hpactor.allocator.region.try_reserve.fail") {
        return false;  // hard-limit rejection
    }
    // ... existing logic ...
}
```

In `record_free()` (around line 87):
```cpp
void record_free(RegionType region, size_t bytes) noexcept {
    FAULT_INJECT("hpactor.allocator.region.record_free.skip") {
        return;  // accounting drift: active_bytes never decrements
    }
    // ... existing logic ...
}
```

- [ ] **Step 5: Build and run tests**

Run: `ninja -C build`
Run: `ctest -R "fault" --output-on-failure`
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/mem/segment_provider.cpp src/mem/slab_cache.cpp src/mem/memory_region.cpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand allocator fault points to 8 wired sites"
```

---

### Task 2.5: Tier 1 integration tests

**Files:**
- Create: `tests/integration/fault/test_fault_scheduler.cpp`
- Create: `tests/integration/fault/test_fault_allocator.cpp`
- Modify: `tests/integration/fault/CMakeLists.txt`

- [ ] **Step 1: Create test_fault_scheduler.cpp**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_schedule.hpp>

#include <gtest/gtest.h>

namespace hpactor {
namespace {

TEST(FaultScheduler, NotifyReadyDropCausesStarvation) {
    ActorSystemConfig cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kScheduler, 0)
            .drop("hpactor.scheduler.notify_ready.drop"));

    system.fault_controller().load(schedule);
    system.fault_controller().enable("*");
    system.fault_controller().install();

    auto actor = system.spawn<TestActor>();
    // Actor is spawned but notify_ready is dropped — actor never runs

    auto snap = system.fault_controller().snapshot();
    EXPECT_GT(snap.faults_fired, 0);
}

TEST(FaultScheduler, TryStealFailDoesNotCrash) {
    ActorSystemConfig cfg;
    cfg.scheduler_threads = 2;
    ActorSystem system(cfg);

    FaultSchedule schedule;
    schedule.add_entry(
        add_entry_to(schedule, FaultDomain::kScheduler, 0)
            .drop("hpactor.scheduler.try_steal.fail"));

    system.fault_controller().load(schedule);
    system.fault_controller().enable("*");

    // Run with scheduler — should not crash or hang
    system.shutdown(ShutdownOptions{});
}

// ... additional tests for each scheduler fault point
```

- [ ] **Step 2: Create test_fault_allocator.cpp similarly**

- [ ] **Step 3: Update CMakeLists.txt**

Add the new test files to `tests/integration/fault/CMakeLists.txt`.

- [ ] **Step 4: Build, run, commit**

Run: `ninja -C build && ctest -R "FaultScheduler|FaultAllocator" --output-on-failure`

```bash
git add tests/integration/fault/test_fault_scheduler.cpp tests/integration/fault/test_fault_allocator.cpp tests/integration/fault/CMakeLists.txt
git commit -m "test(fault): add Tier 1 integration tests for scheduler and allocator"
```

---

## Phase 3 — Tier 2: Resilience Path

### Task 3.1: Actor lifecycle fault points (10 sites)

**Files:**
- Modify: `src/actor/event_based_actor.cpp` (receive already has handler.delay; add become, on_exit, drain_one, circuit_breaker)
- Modify: `src/actor/lifecycle_actor.cpp` (transition)
- Modify: `src/actor/actor_system.cpp` (spawn)
- Modify: `src/fault/fault_points.cpp` (registrations)

- [ ] **Step 1: Add actor lifecycle fault point registrations**

```cpp
const FaultPointRegistrar kActorLifecycleTransitionFail{
    "hpactor.actor.lifecycle.transition.fail", FaultDomain::kActor,
    "Lifecycle state transition CAS always fails"};
const FaultPointRegistrar kActorLifecycleTransitionCorrupt{
    "hpactor.actor.lifecycle.transition.corrupt", FaultDomain::kActor,
    "Lifecycle transitions to wrong state"};
const FaultPointRegistrar kActorReceiveDrop{
    "hpactor.actor.receive.drop", FaultDomain::kActor,
    "Actor receive silently skips message"};
const FaultPointRegistrar kActorBecomeDrop{
    "hpactor.actor.become.drop", FaultDomain::kActor,
    "Behavior swap refused"};
const FaultPointRegistrar kActorOnExitDrop{
    "hpactor.actor.on_exit.drop", FaultDomain::kActor,
    "DownMsg never sent to linked actors"};
const FaultPointRegistrar kActorSpawnFail{
    "hpactor.actor.spawn.fail", FaultDomain::kActor,
    "Actor spawn pipeline failure"};
const FaultPointRegistrar kActorDrainOneCorrupt{
    "hpactor.actor.drain_one.corrupt", FaultDomain::kActor,
    "Wrong drain policy decision"};
const FaultPointRegistrar kActorCircuitBreakerFail{
    "hpactor.actor.circuit_breaker.record.fail", FaultDomain::kActor,
    "Circuit breaker always trips"};
```

- [ ] **Step 2: Wire lifecycle_actor.cpp — transition()**

In `LifecycleActor::transition()` (around line 26):
```cpp
bool LifecycleActor::transition(LifecycleState to) {
    FAULT_INJECT("hpactor.actor.lifecycle.transition.fail") {
        return false;  // CAS always fails
    }
    FAULT_INJECT("hpactor.actor.lifecycle.transition.corrupt") {
        // Transition to a different state than requested
        to = LifecycleState::kFailed;
        // fall through — CAS with wrong target state
    }
    // ... existing CAS logic ...
}
```

- [ ] **Step 3: Wire event_based_actor.cpp — become, on_exit, drain_one, circuit_breaker**

In `become()`:
```cpp
void EventBasedActor::become(Behavior bh) {
    FAULT_INJECT("hpactor.actor.become.drop") {
        return;  // refuse to install new behavior
    }
    // ... existing logic ...
}
```

In `receive()` — add a general drop point before handler dispatch:
```cpp
void EventBasedActor::receive(TypedMessage& msg) {
    FAULT_INJECT("hpactor.actor.receive.drop") {
        return;  // silently skip message
    }
    // ... existing logic (including handler.delay from Task 1.7) ...
}
```

In `on_exit()` (around line 444):
```cpp
void EventBasedActor::on_exit() {
    FAULT_INJECT("hpactor.actor.on_exit.drop") {
        return;  // DownMsg never sent
    }
    // ... existing logic ...
}
```

In `drain_one()` (around line 493):
```cpp
bool EventBasedActor::drain_one(TypedMessage& msg) {
    FAULT_INJECT("hpactor.actor.drain_one.corrupt") {
        return true;  // always process, even during drain
    }
    // ... existing logic ...
}
```

In `record_circuit_breaker_result()` (around line 592):
```cpp
void EventBasedActor::record_circuit_breaker_result(bool success) {
    FAULT_INJECT("hpactor.actor.circuit_breaker.record.fail") {
        success = false;  // always record failure, always trip
    }
    // ... existing logic ...
}
```

- [ ] **Step 4: Wire actor_system.cpp — spawn()**

In `ActorSystem::spawn<T>()` (find the spawn template in actor_system.hpp or the implementation):
```cpp
// Early in spawn, before construction:
FAULT_INJECT("hpactor.actor.spawn.fail") {
    return Actor{};  // return empty actor — spawn failure
}
```

- [ ] **Step 5: Build, test, commit**

Run: `ninja -C build && ctest -R "fault" --output-on-failure`

```bash
git add src/actor/event_based_actor.cpp src/actor/lifecycle_actor.cpp src/actor/actor_system.cpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand actor lifecycle fault points to 10 wired sites"
```

---

### Task 3.2: Supervision fault points (8 sites)

**Files:**
- Modify: `src/supervision/supervision.cpp` (restart_child, handle_child_down, decide_restart, add_child, remove_child)
- Modify: `src/fault/fault_points.cpp` (registrations)

- [ ] **Step 1: Add supervision fault point registrations**

```cpp
const FaultPointRegistrar kSupervisionRestartFail{
    "hpactor.supervision.restart_child.fail", FaultDomain::kSupervision,
    "Restart count never resets, child permanently killed"};
const FaultPointRegistrar kSupervisionHandleDownCorrupt{
    "hpactor.supervision.handle_child_down.corrupt", FaultDomain::kSupervision,
    "Wrong supervision directive dispatched"};
const FaultPointRegistrar kSupervisionDecideRestartFail{
    "hpactor.supervision.decide_restart.fail", FaultDomain::kSupervision,
    "Decide restart always returns Stop"};
const FaultPointRegistrar kSupervisionAddChildDrop{
    "hpactor.supervision.add_child.drop", FaultDomain::kSupervision,
    "Child registration silently refused"};
const FaultPointRegistrar kSupervisionRemoveChildDrop{
    "hpactor.supervision.remove_child.drop", FaultDomain::kSupervision,
    "Stale child reference persists"};
```

- [ ] **Step 2: Wire supervision.cpp sites**

In `SupervisorActor::restart_child()` (around line 100):
```cpp
void SupervisorActor::restart_child(ActorId child, const error& err) {
    FAULT_INJECT("hpactor.supervision.restart_child.drop") {
        return;  // child not restarted (already registered)
    }
    FAULT_INJECT("hpactor.supervision.restart_child.fail") {
        // Increment count past max so child is permanently stopped
        restart_counts_[child] = policy_.max_restarts + 1;
        // fall through
    }
    // ... existing logic ...
}
```

In `handle_child_down()` (around line 63):
```cpp
void SupervisorActor::handle_child_down(TypeTag tag, const StreamBuffer& payload) {
    FAULT_INJECT("hpactor.supervision.handle_child_down.drop") {
        return;  // supervisor never sees child death
    }
    FAULT_INJECT("hpactor.supervision.handle_child_down.corrupt") {
        // Dispatch wrong directive
        // ... modify the directive before switch ...
    }
    // ... existing logic ...
}
```

In `SelfSupervisingActor::decide_restart()` (around line 213):
```cpp
SupervisionDirective SelfSupervisingActor::decide_restart(ActorId child, const error& err) {
    FAULT_INJECT("hpactor.supervision.decide_restart.fail") {
        return SupervisionDirective::Stop;  // always Stop
    }
    // ... existing logic ...
}
```

In `add_child()` and `remove_child()`:
```cpp
void SelfSupervisingActor::add_child(Actor child) {
    FAULT_INJECT("hpactor.supervision.add_child.drop") {
        return;  // silently refuse
    }
    // ... existing logic ...
}

void SelfSupervisingActor::remove_child(Actor child) {
    FAULT_INJECT("hpactor.supervision.remove_child.drop") {
        return;  // leave stale reference
    }
    // ... existing logic ...
}
```

- [ ] **Step 3: Build, test, commit**

```bash
git add src/supervision/supervision.cpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand supervision fault points to 8 wired sites"
```

---

### Task 3.3: Gossip fault points (12 sites)

**Files:**
- Modify: `src/net/gossip_membership.cpp` (async_udp_send, handle_packet, ping, ack, join, protocol_round, mark_suspicious, mark_dead, merge_member, pick_random_peers)
- Modify: `src/fault/fault_points.cpp` (registrations)

- [ ] **Step 1: Add gossip fault point registrations**

```cpp
const FaultPointRegistrar kGossipPingDrop{
    "hpactor.gossip.ping.drop", FaultDomain::kGossip,
    "Gossip ping loss → false suspicion"};
const FaultPointRegistrar kGossipAckDrop{
    "hpactor.gossip.ack.drop", FaultDomain::kGossip,
    "Gossip ack loss → false suspicion cascade"};
const FaultPointRegistrar kGossipJoinDrop{
    "hpactor.gossip.join.drop", FaultDomain::kGossip,
    "Gossip join loss → cluster formation failure"};
const FaultPointRegistrar kGossipSyncCorrupt{
    "hpactor.gossip.sync_rsp.corrupt", FaultDomain::kGossip,
    "Gossip sync response corruption"};
const FaultPointRegistrar kGossipLeaveDrop{
    "hpactor.gossip.leave.drop", FaultDomain::kGossip,
    "Gossip leave message lost"};
const FaultPointRegistrar kGossipProtocolRoundDelay{
    "hpactor.gossip.protocol_round.delay", FaultDomain::kGossip,
    "Gossip protocol round delayed"};
const FaultPointRegistrar kGossipMarkSuspiciousDrop{
    "hpactor.gossip.mark_suspicious.drop", FaultDomain::kGossip,
    "Gossip mark suspicious skipped"};
const FaultPointRegistrar kGossipMarkDeadDrop{
    "hpactor.gossip.mark_dead.drop", FaultDomain::kGossip,
    "Gossip mark dead skipped"};
const FaultPointRegistrar kGossipMergeMemberCorrupt{
    "hpactor.gossip.merge_member.corrupt", FaultDomain::kGossip,
    "Gossip merge member incarnation corruption"};
const FaultPointRegistrar kGossipPickPeersFail{
    "hpactor.gossip.pick_random_peers.fail", FaultDomain::kGossip,
    "Gossip pick random peers returns empty"};
```

- [ ] **Step 2: Wire gossip_membership.cpp sites**

Follow the existing `hpactor.gossip.packet.loss` pattern (wired in Task 1.7) to add FAULT_INJECT at each gossip method entry. Key sites:

In `send_ping()`:
```cpp
FAULT_INJECT("hpactor.gossip.ping.drop") { return; }
```

In `send_ack()`:
```cpp
FAULT_INJECT("hpactor.gossip.ack.drop") { return; }
```

In `send_join()`:
```cpp
FAULT_INJECT("hpactor.gossip.join.drop") { return; }
```

In `handle_sync_rsp()`:
```cpp
FAULT_INJECT("hpactor.gossip.sync_rsp.corrupt") { /* modify table */ }
```

In `send_leave()`:
```cpp
FAULT_INJECT("hpactor.gossip.leave.drop") { return; }
```

In `protocol_round()`:
```cpp
FAULT_INJECT("hpactor.gossip.protocol_round.delay") {
    _fc->stall(FaultDomain::kGossip, /*delay_ticks=*/3);
}
```

In `mark_suspicious()`:
```cpp
FAULT_INJECT("hpactor.gossip.mark_suspicious.drop") { return; }
```

In `mark_dead()`:
```cpp
FAULT_INJECT("hpactor.gossip.mark_dead.drop") { return; }
```

In `merge_member()`:
```cpp
FAULT_INJECT("hpactor.gossip.merge_member.corrupt") {
    // Corrupt incarnation number
    member.incarnation += 100;
    // fall through
}
```

In `pick_random_peers()`:
```cpp
FAULT_INJECT("hpactor.gossip.pick_random_peers.fail") {
    result.clear();  // return empty set
    return;
}
```

- [ ] **Step 3: Build, test, commit**

```bash
git add src/net/gossip_membership.cpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand gossip fault points to 12 wired sites"
```

---

### Task 3.4: RPC and Discovery fault points (13 sites)

**Files:**
- Modify: `src/rpc/rpc_channel.cpp` (send, response, timeout, retry)
- Modify: `src/net/registrar_client.cpp` (heartbeat, register, connect)
- Modify: `src/net/actor_location_cache.cpp` (get, put, evict)
- Modify: `src/fault/fault_points.cpp` (registrations)

- [ ] **Step 1: Add RPC and Discovery fault point registrations**

```cpp
// RPC
const FaultPointRegistrar kRpcSendDelay{"hpactor.rpc.send.delay", FaultDomain::kRpc, "RPC send delayed"};
const FaultPointRegistrar kRpcResponseDelay{"hpactor.rpc.response.delay", FaultDomain::kRpc, "RPC response delayed"};
const FaultPointRegistrar kRpcTimeoutDrop{"hpactor.rpc.timeout.drop", FaultDomain::kRpc, "RPC timeout not processed"};
const FaultPointRegistrar kRpcRetryDrop{"hpactor.rpc.retry.drop", FaultDomain::kRpc, "RPC retry never scheduled"};

// Discovery
const FaultPointRegistrar kDiscoveryConnectFail{"hpactor.discovery.connect.fail", FaultDomain::kDiscovery, "Registrar connection failure"};
const FaultPointRegistrar kLocationCacheGetFail{"hpactor.location_cache.get.fail", FaultDomain::kDiscovery, "Location cache always misses"};
const FaultPointRegistrar kLocationCacheGetCorrupt{"hpactor.location_cache.get.corrupt", FaultDomain::kDiscovery, "Location cache returns wrong endpoint"};
const FaultPointRegistrar kLocationCachePutDrop{"hpactor.location_cache.put.drop", FaultDomain::kDiscovery, "Location cache put silently dropped"};
const FaultPointRegistrar kLocationCacheEvictDrop{"hpactor.location_cache.evict.drop", FaultDomain::kDiscovery, "Location cache stale entry persists"};
```

- [ ] **Step 2: Wire rpc_channel.cpp**

In `send_request()`:
```cpp
FAULT_INJECT("hpactor.rpc.send.delay") {
    _fc->stall(FaultDomain::kRpc, /*delay_ticks=*/5);
}
```

In `on_response()`:
```cpp
FAULT_INJECT("hpactor.rpc.response.delay") {
    _fc->stall(FaultDomain::kRpc, /*delay_ticks=*/3);
}
```

In `on_timeout()`:
```cpp
FAULT_INJECT("hpactor.rpc.timeout.drop") { return; }
```

In `schedule_retry()`:
```cpp
FAULT_INJECT("hpactor.rpc.retry.drop") { return; }
```

- [ ] **Step 3: Wire registrar_client.cpp and actor_location_cache.cpp sites**

Similar pattern — FAULT_INJECT at each method entry with appropriate action.

- [ ] **Step 4: Build, test, commit**

```bash
git add src/rpc/rpc_channel.cpp src/net/registrar_client.cpp src/net/actor_location_cache.cpp src/fault/fault_points.cpp
git commit -m "feat(fault): expand RPC and discovery fault points to 13 wired sites"
```

---

### Task 3.5: Tier 2 integration tests

**Files:**
- Create: `tests/integration/fault/test_fault_actor_lifecycle.cpp`
- Create: `tests/integration/fault/test_fault_gossip.cpp`
- Create: `tests/integration/fault/test_fault_rpc.cpp`
- Modify: `tests/integration/fault/CMakeLists.txt`

Follow the test patterns from the spec (Section 4). Each test file covers the corresponding subsystem's fault points with deterministic schedule-based tests.

Build, test, commit after each test file.

---

## Phase 4 — Tier 3: Observability Path

### Task 4.1: Tracing fault points (10 sites)

**Files:**
- Modify: `src/tracing/trace_manager.cpp` (start_span, finish_span, inject_context, drain_once, force_flush, start)
- Modify: `src/tracing/trace_exporter.cpp` (export_batch)
- Modify: `include/hpactor/tracing/sampler.hpp` (should_sample)
- Modify: `src/fault/fault_points.cpp` (registrations)

Wire FAULT_INJECT at each method entry with appropriate action (Drop for span ops, Fail for export, Corrupt for context injection/sampler).

### Task 4.2: Metrics fault points (7 sites)

**Files:**
- Modify: `src/metrics/metrics_aggregator.cpp` (on_event)
- Modify: `include/hpactor/metrics/metrics_registry.hpp` (snapshot, register_family)
- Modify: `include/hpactor/metrics/metrics_formatter.hpp` (format)
- Modify: `src/fault/fault_points.cpp` (registrations)

### Task 4.3: CLI and Config fault points (14 sites)

**Files:**
- Modify: `src/cli/cli_actor.cpp` (run_once, execute_tokens, inspect_request)
- Modify: `src/cli/lexer.cpp` (tokenize)
- Modify: `src/cli/command_node.cpp` (find_child)
- Modify: `src/config/toml_parser.cpp` (parse)
- Modify: `src/config/actor_factory_registry.cpp` (get_factory)
- Modify: `src/config/binary_loader.cpp` (load)
- Modify: `src/fault/fault_points.cpp` (registrations)

### Task 4.4: Tier 3 integration tests

**Files:**
- Create: `tests/integration/fault/test_fault_tracing.cpp`
- Create: `tests/integration/fault/test_fault_chaos_scenario.cpp`
- Create: `tests/system/fault/test_fault_end_to_end.cpp`
- Modify: CMakeLists.txt files

### Task 4.5: Final verification — full build and test

- [ ] **Step 1: Build with ENABLE_FAULT_INJECTION=ON**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```

- [ ] **Step 2: Run all tests**

```bash
ctest --output-on-failure --parallel 8
```
Expected: All existing tests pass. New fault tests pass.

- [ ] **Step 3: Build with ENABLE_FAULT_INJECTION=OFF**

```bash
cmake -S . -B build-off -GNinja -DENABLE_FAULT_INJECTION=OFF
ninja -C build-off
```

- [ ] **Step 4: Run all tests with fault injection disabled**

```bash
ctest --test-dir build-off --output-on-failure --parallel 8
```
Expected: All tests pass. FAULT_INJECT overhead is zero.

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "feat(fault): complete Tier 3 observability fault points

Full expansion: ~117 FAULT_INJECT sites across all 13 subsystems,
14 fault domains, per-thread controller instances, expand_random()
helper, and fault timeline logging.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Summary

| Phase | Deliverable | Wired Sites | New Test Files |
|-------|-------------|-------------|----------------|
| 1 | Infrastructure + 9 unwired points | 12 (up from 3) | 3 (unit) |
| 2 | Tier 1 core path | ~55 | 2 (integration) |
| 3 | Tier 2 resilience path | ~98 | 3 (integration) |
| 4 | Tier 3 observability path | ~117 | 3 (integration + system) |
