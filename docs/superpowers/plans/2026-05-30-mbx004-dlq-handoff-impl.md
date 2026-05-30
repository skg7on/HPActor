# MBX-004: Dead-Letter Handoff Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the dead-letter handoff for mailbox overflow policy `DeadLetter` so DLQ records carry payload, trace context, and timestamp; add CLI `/dlq` commands; remove dead config.

**Architecture:** Eight tasks. Core fix: extract payload + trace from `TypedMessage` before the `try_push()` move, pass them to `emit_rejection_observability()` for DLQ record construction — matching the pattern in `reject_missing_actor` and `try_reject_expired`. CLI commands auto-register via `CommandRegistration<T>` file-scope objects (same pattern as fault_commands.cpp). `to_string` for `DeadLetterReason`/`DeadLetterSource` added as inline functions in `dead_letter_queue.hpp`.

**Tech Stack:** C++20, Google Test, CMake/Ninja

**Spec:** `docs/superpowers/specs/2026-05-30-mbx004-dlq-handoff-design.md`

---

### Task 1: DeadLetterQueue API — config(), snapshot_records(), try_pop_at()

**Files:**
- Modify: `include/hpactor/mailbox/dead_letter_queue.hpp`
- Modify: `src/mailbox/dead_letter_queue.cpp`
- Modify: `tests/unit/mailbox/test_dead_letter_queue.cpp`

- [ ] **Step 1: Add methods to header**

In `include/hpactor/mailbox/dead_letter_queue.hpp`, add `#include <vector>` at the top. Then in the `DeadLetterQueue` class body (after line 188, before `private:`), add:

```cpp
    const DeadLetterConfig& config() const noexcept { return config_; }

    std::vector<DeadLetterRecord> snapshot_records() const;

    bool try_pop_at(size_t index, DeadLetterRecord& out) noexcept;
```

- [ ] **Step 2: Implement new methods**

In `src/mailbox/dead_letter_queue.cpp`, add after the `snapshot()` method (after line 88):

```cpp
std::vector<DeadLetterRecord> DeadLetterQueue::snapshot_records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<DeadLetterRecord>(records_.begin(), records_.end());
}

bool DeadLetterQueue::try_pop_at(size_t index, DeadLetterRecord& out) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= records_.size()) {
        return false;
    }
    auto it = records_.begin();
    std::advance(it, static_cast<long>(index));
    out = std::move(*it);
    records_.erase(it);
    total_popped_++;
    return true;
}
```

- [ ] **Step 3: Add unit tests**

At the end of `tests/unit/mailbox/test_dead_letter_queue.cpp`, add:

```cpp
TEST(DeadLetterQueueTest, SnapshotRecordsReturnsCopy) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord a; a.message_id = 1; q.try_push(std::move(a));
    DeadLetterRecord b; b.message_id = 2; q.try_push(std::move(b));

    auto records = q.snapshot_records();
    EXPECT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].message_id, 1u);
    EXPECT_EQ(records[1].message_id, 2u);

    // snapshot_records does not remove
    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 2u);
}

TEST(DeadLetterQueueTest, TryPopAtRemovesCorrectElement) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    for (uint64_t i = 0; i < 5; ++i) {
        DeadLetterRecord r; r.message_id = i; q.try_push(std::move(r));
    }

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop_at(2, out));
    EXPECT_EQ(out.message_id, 2u);

    // Remaining: 0, 1, 3, 4
    EXPECT_TRUE(q.try_pop(out));  EXPECT_EQ(out.message_id, 0u);
    EXPECT_TRUE(q.try_pop(out));  EXPECT_EQ(out.message_id, 1u);
    EXPECT_TRUE(q.try_pop(out));  EXPECT_EQ(out.message_id, 3u);
    EXPECT_TRUE(q.try_pop(out));  EXPECT_EQ(out.message_id, 4u);

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 0u);
    EXPECT_EQ(snap.total_popped, 5u);
}

TEST(DeadLetterQueueTest, TryPopAtOutOfBoundsReturnsFalse) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord r; r.message_id = 1; q.try_push(std::move(r));

    DeadLetterRecord out;
    EXPECT_FALSE(q.try_pop_at(1, out));
    EXPECT_FALSE(q.try_pop_at(100, out));

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 1u);
}

TEST(DeadLetterQueueTest, ConfigAccessorReturnsReference) {
    DeadLetterConfig cfg;
    cfg.capacity = 1234;
    cfg.enabled = false;
    DeadLetterQueue q(cfg);

    EXPECT_EQ(q.config().capacity, 1234u);
    EXPECT_EQ(q.config().enabled, false);
}
```

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build tests/unit/mailbox/test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="DeadLetterQueue*"
```

Expected: All 5 DeadLetterQueue tests pass (1 existing + 4 new).

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/dead_letter_queue.hpp \
        src/mailbox/dead_letter_queue.cpp \
        tests/unit/mailbox/test_dead_letter_queue.cpp
git commit -m "feat(mailbox): add config(), snapshot_records(), try_pop_at() to DeadLetterQueue"
```

---

### Task 2: Remove write-only enable_dead_letters from MailboxConfig

**Files:**
- Modify: `include/hpactor/mailbox/mailbox_policy.hpp`
- Modify: `tests/unit/mailbox/test_mailbox_policy.cpp`

- [ ] **Step 1: Remove field**

In `include/hpactor/mailbox/mailbox_policy.hpp`, delete line 70:
```cpp
    bool enable_dead_letters = true;
```

- [ ] **Step 2: Remove test assertion**

In `tests/unit/mailbox/test_mailbox_policy.cpp`, delete line 34:
```cpp
    EXPECT_EQ(cfg.enable_dead_letters, true);
```

- [ ] **Step 3: Build and run policy test**

```bash
ninja -C build tests/unit/mailbox/test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="MailboxPolicy*"
```

Expected: All MailboxPolicy tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/mailbox_policy.hpp \
        tests/unit/mailbox/test_mailbox_policy.cpp
git commit -m "refactor(mailbox): remove write-only enable_dead_letters from MailboxConfig"
```

---

### Task 3: Add dlq pointer to OverflowContext

**Files:**
- Modify: `include/hpactor/mailbox/detail/overflow_context.hpp`

- [ ] **Step 1: Add forward declaration and field**

In `include/hpactor/mailbox/detail/overflow_context.hpp`, add before the `namespace hpactor::mailbox::detail` block:

```cpp
namespace hpactor::mailbox {
class DeadLetterQueue;
}
```

Then add at the end of the `OverflowContext` struct (after `drop_oldest_fn`):

```cpp
    mailbox::DeadLetterQueue* dlq = nullptr;
```

The full struct becomes:

```cpp
template <typename T> struct OverflowContext {
    const T& message;
    MailboxEnvelopeMeta& meta;
    ReservationManager<T>& reservation;
    OverflowQueue<T>& overflow_queue;
    std::atomic<uint64_t>& total_rejected;
    std::atomic<uint64_t>& total_dropped;
    std::atomic<uint64_t>& total_dead_letters;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_buf;
    MailboxConfig& config;
    ActorId actor_id;
    uint32_t current_depth;
    uint64_t current_bytes;
    std::function<bool()> drop_oldest_fn;
    mailbox::DeadLetterQueue* dlq = nullptr;
};
```

The `nullptr` default ensures existing code that constructs `OverflowContext` with positional aggregate init continues to compile (trailing unspecified elements are default-initialized in C++20).

- [ ] **Step 2: Build overflow handler test to verify**

```bash
ninja -C build tests/unit/mailbox/test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="OverflowHandler*"
```

Expected: All 9 OverflowHandler tests pass.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/mailbox/detail/overflow_context.hpp
git commit -m "feat(mailbox): add dlq pointer to OverflowContext for future handler use"
```

---

### Task 4: Fix DLQ record completeness — payload, trace, timestamp, config gate (G1, G5)

**Files:**
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add timestamp and trace to reject_missing_actor**

In `src/actor/actor_system.cpp`, in `reject_missing_actor()` at line ~613 (`dl.payload_sample = msg.payload();`), replace:

```cpp
        dl.payload_sample = msg.payload();
        (void)dlq->try_push(std::move(dl));
```

with:

```cpp
        dl.payload_sample = msg.payload();
        dl.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (msg.has_trace_context()) {
            const auto& tc = msg.trace_context();
            dl.trace_id_hi = tc.trace_id_hi;
            dl.trace_id_lo = tc.trace_id_lo;
            dl.span_id = tc.span_id;
        }
        (void)dlq->try_push(std::move(dl));
```

- [ ] **Step 2: Add timestamp and trace to try_reject_expired**

In `try_reject_expired()` at line ~717 (`dl.payload_sample = msg.payload();`), replace:

```cpp
        dl.payload_sample = msg.payload();
        (void)dlq->try_push(std::move(dl));
```

with:

```cpp
        dl.payload_sample = msg.payload();
        dl.timestamp_ns = now_ns;
        if (msg.has_trace_context()) {
            const auto& tc = msg.trace_context();
            dl.trace_id_hi = tc.trace_id_hi;
            dl.trace_id_lo = tc.trace_id_lo;
            dl.span_id = tc.span_id;
        }
        (void)dlq->try_push(std::move(dl));
```

(`now_ns` is already computed at line ~683.)

- [ ] **Step 3: Rewrite emit_rejection_observability to accept payload + trace and add config gate**

First, update the function signature. Replace the existing `emit_rejection_observability` declaration (lines 726-732):

```cpp
void emit_rejection_observability(hpactor::mailbox::DeadLetterQueue* dlq,
                                  MetricBuf* metrics, hpactor::EndPoint endpoint,
                                  hpactor::ActorId target,
                                  const hpactor::mailbox::MailboxEnvelopeMeta& meta,
                                  const hpactor::mailbox::EnqueueResult& result,
                                  const hpactor::mailbox::DeliveryOptions& options,
                                  hpactor::mailbox::OverflowPolicy overflow_policy) {
```

with:

```cpp
void emit_rejection_observability(hpactor::mailbox::DeadLetterQueue* dlq,
                                  MetricBuf* metrics, hpactor::EndPoint endpoint,
                                  hpactor::ActorId target,
                                  const hpactor::StreamBuffer& msg_payload,
                                  const hpactor::TraceContext& msg_trace,
                                  bool msg_has_trace,
                                  const hpactor::mailbox::MailboxEnvelopeMeta& meta,
                                  const hpactor::mailbox::EnqueueResult& result,
                                  const hpactor::mailbox::DeliveryOptions& options,
                                  hpactor::mailbox::OverflowPolicy overflow_policy) {
```

Then replace the DLQ section within the function body (lines 738-752). Replace:

```cpp
    if (dlq && overflow_policy == hpactor::mailbox::OverflowPolicy::DeadLetter) {
        hpactor::mailbox::DeadLetterRecord dl;
        dl.reason = hpactor::mailbox::DeadLetterReason::OverflowPolicy;
        dl.source = hpactor::mailbox::DeadLetterSource::MailboxAdmission;
        dl.sender = meta.sender;
        dl.target =
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0};
        dl.type_tag = meta.type_tag;
        dl.message_id = meta.message_id;
        dl.frame_flags = meta.flags;
        dl.priority = meta.priority;
        dl.deadline_ns = meta.deadline_ns;
        dl.mailbox_depth = result.depth;
        dl.mailbox_capacity = result.capacity;
        (void)dlq->try_push(std::move(dl));
    }
```

with:

```cpp
    if (dlq && dlq->config().enabled &&
        overflow_policy == hpactor::mailbox::OverflowPolicy::DeadLetter) {
        hpactor::mailbox::DeadLetterRecord dl;
        dl.reason = hpactor::mailbox::DeadLetterReason::OverflowPolicy;
        dl.source = hpactor::mailbox::DeadLetterSource::MailboxAdmission;
        dl.sender = meta.sender;
        dl.target =
            hpactor::ActorAddress{endpoint, hpactor::ActorType{0}, target, 0};
        dl.type_tag = meta.type_tag;
        dl.message_id = meta.message_id;
        dl.frame_flags = meta.flags;
        dl.priority = meta.priority;
        dl.deadline_ns = meta.deadline_ns;
        dl.mailbox_depth = result.depth;
        dl.mailbox_capacity = result.capacity;
        dl.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        dl.payload_sample = msg_payload;
        if (msg_has_trace) {
            dl.trace_id_hi = msg_trace.trace_id_hi;
            dl.trace_id_lo = msg_trace.trace_id_lo;
            dl.span_id = msg_trace.span_id;
        }
        (void)dlq->try_push(std::move(dl));
    }
```

- [ ] **Step 4: Update call site in try_deliver_local**

In `try_deliver_local()` at lines ~822-828, replace:

```cpp
    const auto bp_mode = mailbox->config().backpressure_mode;
    auto result = mailbox->try_push(std::move(msg), meta);

    emit_rejection_observability(dead_letters_.get(), metrics_ring_buffer_.get(),
                                 endpoint_, target, meta, result, options,
                                 mailbox->config().overflow_policy);
```

with:

```cpp
    const auto bp_mode = mailbox->config().backpressure_mode;
    StreamBuffer msg_payload = msg.payload();
    TraceContext msg_trace;
    bool msg_has_trace = msg.has_trace_context();
    if (msg_has_trace) {
        msg_trace = msg.trace_context();
    }
    auto result = mailbox->try_push(std::move(msg), meta);

    emit_rejection_observability(dead_letters_.get(), metrics_ring_buffer_.get(),
                                 endpoint_, target, msg_payload, msg_trace,
                                 msg_has_trace, meta, result, options,
                                 mailbox->config().overflow_policy);
```

Add `#include <hpactor/adt/stream_buffer.hpp>` if not already transitively included (it already is via `typed_message.hpp` → `mailbox.hpp` path).

- [ ] **Step 5: Build to verify compilation**

```bash
ninja -C build
```

Expected: Clean build.

- [ ] **Step 6: Run existing DLQ-related tests**

```bash
ctest -R "dead_letter|mailbox_overflow|mailbox_policy|backpressure" --output-on-failure -j8
```

Expected: All existing tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/actor/actor_system.cpp
git commit -m "fix(mailbox): preserve payload, trace context, and timestamp in DLQ overflow records

Extract message payload and trace context before moving msg into mailbox,
then pass them to emit_rejection_observability for DLQ record creation.
Add DeadLetterConfig::enabled gate at call site.
Add timestamp and trace context to reject_missing_actor and try_reject_expired
DLQ paths for consistency across all three dead-letter sources."
```

---

### Task 5: Add dead_letter_queue() accessor to ActorSystem

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`

- [ ] **Step 1: Add accessor**

In `include/hpactor/core/actor_system.hpp`, after the existing `pop_dead_letter` method (~line 469), add:

```cpp
    /// \brief Direct access to the dead-letter queue.
    ///
    /// Returns nullptr if dead-letter queue is not initialized.
    mailbox::DeadLetterQueue* dead_letter_queue() noexcept {
        return dead_letters_.get();
    }
    const mailbox::DeadLetterQueue* dead_letter_queue() const noexcept {
        return dead_letters_.get();
    }
```

- [ ] **Step 2: Build to verify**

```bash
ninja -C build
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/core/actor_system.hpp
git commit -m "feat(core): add dead_letter_queue() accessor to ActorSystem"
```

---

### Task 6: Add to_string for DeadLetterReason and DeadLetterSource

**Files:**
- Modify: `include/hpactor/mailbox/dead_letter_queue.hpp`
- Modify: `tests/unit/mailbox/test_dead_letter_failure.cpp`

- [ ] **Step 1: Add to_string inline functions**

In `include/hpactor/mailbox/dead_letter_queue.hpp`, add after the existing `failure_source()` constexpr function (~line 112):

```cpp
[[nodiscard]] inline const char* to_string(DeadLetterReason reason) noexcept {
    switch (reason) {
        case DeadLetterReason::MailboxFull:           return "MailboxFull";
        case DeadLetterReason::MailboxClosed:          return "MailboxClosed";
        case DeadLetterReason::ActorNotFound:          return "ActorNotFound";
        case DeadLetterReason::ActorTerminated:        return "ActorTerminated";
        case DeadLetterReason::MissingRoute:           return "MissingRoute";
        case DeadLetterReason::RemoteNodeUnreachable:  return "RemoteNodeUnreachable";
        case DeadLetterReason::NetworkPartition:       return "NetworkPartition";
        case DeadLetterReason::TransportSendFailed:    return "TransportSendFailed";
        case DeadLetterReason::DecodeFailed:           return "DecodeFailed";
        case DeadLetterReason::OverflowPolicy:         return "OverflowPolicy";
        case DeadLetterReason::NoDropRejected:         return "NoDropRejected";
        case DeadLetterReason::DrainTimeout:           return "DrainTimeout";
        case DeadLetterReason::DrainPolicyDrop:        return "DrainPolicyDrop";
        case DeadLetterReason::Expired:                return "Expired";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* to_string(DeadLetterSource source) noexcept {
    switch (source) {
        case DeadLetterSource::LocalDelivery:      return "LocalDelivery";
        case DeadLetterSource::RemoteDelivery:     return "RemoteDelivery";
        case DeadLetterSource::ActorProxy:         return "ActorProxy";
        case DeadLetterSource::Transport:          return "Transport";
        case DeadLetterSource::MailboxAdmission:   return "MailboxAdmission";
        case DeadLetterSource::ServiceDiscovery:   return "ServiceDiscovery";
        case DeadLetterSource::Replay:             return "Replay";
    }
    return "Unknown";
}
```

Also add `#include <cstdint>` if not already present (it already is at line 23).

- [ ] **Step 2: Add unit tests**

At the end of `tests/unit/mailbox/test_dead_letter_failure.cpp`, add:

```cpp
TEST(DeadLetterFailureTest, ToStringDeadLetterReason) {
    using namespace hpactor::mailbox;
    EXPECT_STREQ(to_string(DeadLetterReason::MailboxFull), "MailboxFull");
    EXPECT_STREQ(to_string(DeadLetterReason::OverflowPolicy), "OverflowPolicy");
    EXPECT_STREQ(to_string(DeadLetterReason::Expired), "Expired");
    EXPECT_STREQ(to_string(DeadLetterReason::DrainTimeout), "DrainTimeout");
}

TEST(DeadLetterFailureTest, ToStringDeadLetterSource) {
    using namespace hpactor::mailbox;
    EXPECT_STREQ(to_string(DeadLetterSource::LocalDelivery), "LocalDelivery");
    EXPECT_STREQ(to_string(DeadLetterSource::MailboxAdmission), "MailboxAdmission");
    EXPECT_STREQ(to_string(DeadLetterSource::Replay), "Replay");
}
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build tests/unit/mailbox/test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="DeadLetterFailure*"
```

Expected: All DeadLetterFailure tests pass (4 existing + 2 new).

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/dead_letter_queue.hpp \
        tests/unit/mailbox/test_dead_letter_failure.cpp
git commit -m "feat(mailbox): add to_string for DeadLetterReason and DeadLetterSource"
```

---

### Task 7: CLI /dlq commands

**Files:**
- Create: `src/cli/commands/dlq_commands.hpp`
- Create: `src/cli/commands/dlq_commands.cpp`

- [ ] **Step 1: Create header**

Create `src/cli/commands/dlq_commands.hpp`:

```cpp
#pragma once

// CLI commands for dead-letter queue: /dlq list, /dlq show, /dlq replay, /dlq export.
// Auto-registered via file-scope CommandRegistration objects.
```

- [ ] **Step 2: Create implementation — helpers and /dlq list**

Create `src/cli/commands/dlq_commands.cpp`:

```cpp
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {
namespace {

// ── helpers ──────────────────────────────────────────────────────────────

std::string format_age_ns(uint64_t timestamp_ns) {
    if (timestamp_ns == 0) return "-";
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    uint64_t age_s = (now_ns - timestamp_ns) / 1'000'000'000ULL;
    if (age_s < 60) return std::to_string(age_s) + "s";
    if (age_s < 3600) return std::to_string(age_s / 60) + "m";
    return std::to_string(age_s / 3600) + "h";
}

std::string type_tag_hex(TypeTag tag) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::setfill('0') << std::setw(4)
       << static_cast<uint16_t>(tag);
    return ss.str();
}

std::string trace_id_str(uint64_t hi, uint64_t lo) {
    if (hi == 0 && lo == 0) return "(none)";
    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(16) << hi << std::setw(16) << lo;
    return ss.str();
}

// ── /dlq list ────────────────────────────────────────────────────────────

class DlqListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "dlq/list"; }
    std::string_view help_text() const noexcept override {
        return "List dead-letter queue records";
    }
    int order() const noexcept override { return 500; }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }

        auto records = dlq->snapshot_records();
        auto reason_filter = ctx.get_param("reason");
        auto source_filter = ctx.get_param("source");
        uint32_t limit_val = 50;
        if (auto lim = ctx.get_param("limit")) {
            limit_val = static_cast<uint32_t>(std::stoul(*lim));
        }

        std::vector<size_t> filtered;
        for (size_t i = 0; i < records.size(); ++i) {
            auto& r = records[i];
            if (reason_filter &&
                std::string(mailbox::to_string(r.reason)) != *reason_filter)
                continue;
            if (source_filter &&
                std::string(mailbox::to_string(r.source)) != *source_filter)
                continue;
            filtered.push_back(i);
        }

        ctx.output->header("Dead-Letter Queue Records (" +
                          std::to_string(filtered.size()) + " total)");

        std::vector<std::string> cols = {"#", "Reason", "Source",
                                         "Target", "TypeTag", "Age"};
        std::vector<std::vector<std::string>> rows;
        for (size_t j = 0; j < filtered.size() && j < limit_val; ++j) {
            size_t idx = filtered[j];
            auto& r = records[idx];
            rows.push_back({
                std::to_string(idx),
                std::string(mailbox::to_string(r.reason)),
                std::string(mailbox::to_string(r.source)),
                std::string(to_string(r.target.id)),
                type_tag_hex(r.type_tag),
                format_age_ns(r.timestamp_ns),
            });
        }
        ctx.output->table(cols, rows);

        if (filtered.size() > limit_val) {
            ctx.output->raw("... " +
                           std::to_string(filtered.size() - limit_val) +
                           " more (use --limit to show more)");
        }
        return result<void>::make();
    }
};

// ── /dlq show <index> ────────────────────────────────────────────────────

class DlqShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "dlq/show"; }
    std::string_view help_text() const noexcept override {
        return "Show a dead-letter record: /dlq show --index N";
    }
    int order() const noexcept override { return 510; }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }
        auto idx_str = ctx.get_param("index");
        if (!idx_str) {
            ctx.output->error("Usage: /dlq show --index N");
            return result<void>::make();
        }
        size_t index = std::stoul(*idx_str);

        auto records = dlq->snapshot_records();
        if (index >= records.size()) {
            ctx.output->error("Index " + std::to_string(index) +
                             " out of range (0.." +
                             std::to_string(records.size() - 1) + ")");
            return result<void>::make();
        }

        auto& r = records[index];
        std::map<std::string, std::string> kv;
        kv["Index"]        = std::to_string(index);
        kv["Reason"]       = std::string(mailbox::to_string(r.reason));
        kv["Source"]       = std::string(mailbox::to_string(r.source));
        kv["Sender"]       = std::string(to_string(r.sender.id));
        kv["Target"]       = std::string(to_string(r.target.id));
        kv["TypeTag"]      = type_tag_hex(r.type_tag);
        kv["MessageID"]    = std::to_string(r.message_id);
        kv["Priority"]     = std::to_string(r.priority);
        kv["Payload size"] = std::to_string(r.payload_size) + " bytes";
        kv["Mailbox"]      = std::to_string(r.mailbox_depth) + "/" +
                             std::to_string(r.mailbox_capacity);
        kv["Age"]          = format_age_ns(r.timestamp_ns);
        kv["TraceID"]      = trace_id_str(r.trace_id_hi, r.trace_id_lo);

        ctx.output->header("Dead-Letter Record #" + std::to_string(index));
        ctx.output->key_value(kv);

        // Payload hex dump (first 128 bytes)
        if (r.payload_size > 0 && !r.payload_sample.empty()) {
            std::stringstream hex;
            const auto& buf = r.payload_sample;
            size_t show = std::min<size_t>(128, buf.size());
            for (size_t i = 0; i < show; ++i) {
                if (i > 0 && i % 16 == 0) hex << "\n";
                hex << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<int>(buf[i]) << " ";
            }
            ctx.output->raw("Payload hex (first 128 bytes):");
            ctx.output->raw(hex.str());
        }

        // Failure envelope summary
        auto env = r.to_failure_envelope();
        std::map<std::string, std::string> env_kv;
        env_kv["FailureReason"] = std::string(to_string(env.reason));
        env_kv["FailureSource"] = std::string(to_string(env.source));
        env_kv["Retryable"] = env.retryable ? "yes" : "no";
        ctx.output->key_value(env_kv);

        return result<void>::make();
    }
};

// ── /dlq replay <index> ──────────────────────────────────────────────────

class DlqReplayCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "dlq/replay"; }
    std::string_view help_text() const noexcept override {
        return "Replay a dead-letter record: /dlq replay --index N";
    }
    int order() const noexcept override { return 520; }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }
        auto idx_str = ctx.get_param("index");
        if (!idx_str) {
            ctx.output->error("Usage: /dlq replay --index N");
            return result<void>::make();
        }
        size_t index = std::stoul(*idx_str);

        DeadLetterRecord r;
        if (!dlq->try_pop_at(index, r)) {
            ctx.output->error("Index " + std::to_string(index) +
                             " out of range");
            return result<void>::make();
        }

        if (r.payload_sample.empty()) {
            ctx.output->error("Record has no payload — cannot replay");
            return result<void>::make();
        }

        // Reconstruct TypedMessage from stored payload and deliver.
        TypedMessage msg(r.type_tag, r.payload_sample);
        system->deliver_local(r.target.id, std::move(msg));
        ctx.output->raw("Replayed record #" + std::to_string(index) +
                       " to actor " + std::string(to_string(r.target.id)));
        return result<void>::make();
    }
};

// ── /dlq export ──────────────────────────────────────────────────────────

class DlqExportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override { return "dlq/export"; }
    std::string_view help_text() const noexcept override {
        return "Export dead-letter records: /dlq export [--format json|text]";
    }
    int order() const noexcept override { return 530; }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }

        auto records = dlq->snapshot_records();
        uint32_t limit_val = 100;
        if (auto lim = ctx.get_param("limit")) {
            limit_val = static_cast<uint32_t>(std::stoul(*lim));
        }
        bool json = ctx.get_param("format").value_or("text") == "json";

        if (json) {
            ctx.output->raw("[");
            for (size_t i = 0; i < records.size() && i < limit_val; ++i) {
                auto& r = records[i];
                std::stringstream ss;
                ss << "  {"
                   << "\"reason\":\"" << mailbox::to_string(r.reason) << "\","
                   << "\"source\":\"" << mailbox::to_string(r.source) << "\","
                   << "\"target\":\"" << to_string(r.target.id) << "\","
                   << "\"type_tag\":\"" << type_tag_hex(r.type_tag) << "\","
                   << "\"message_id\":" << r.message_id << ","
                   << "\"payload_size\":" << r.payload_size
                   << "}";
                if (i + 1 < records.size() && i + 1 < limit_val) ss << ",";
                ctx.output->raw(ss.str());
            }
            ctx.output->raw("]");
        } else {
            for (size_t i = 0; i < records.size() && i < limit_val; ++i) {
                auto& r = records[i];
                std::stringstream ss;
                ss << i << " "
                   << mailbox::to_string(r.reason) << " "
                   << mailbox::to_string(r.source) << " "
                   << to_string(r.target.id) << " "
                   << type_tag_hex(r.type_tag) << " "
                   << r.payload_size << "B";
                ctx.output->raw(ss.str());
            }
        }
        return result<void>::make();
    }
};

// ── registration ─────────────────────────────────────────────────────────

const CommandRegistration<DlqListCommand>   kRegisterDlqList;
const CommandRegistration<DlqShowCommand>   kRegisterDlqShow;
const CommandRegistration<DlqReplayCommand> kRegisterDlqReplay;
const CommandRegistration<DlqExportCommand> kRegisterDlqExport;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
```

- [ ] **Step 3: Verify the file compiles**

```bash
ninja -C build src/cli/commands/dlq_commands.cpp.o
```

If the object file target name differs (CMake may glob), use:

```bash
ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add src/cli/commands/dlq_commands.hpp src/cli/commands/dlq_commands.cpp
git commit -m "feat(cli): add /dlq list, show, replay, and export commands"
```

---

### Task 8: Integration and end-to-end tests

**Files:**
- Modify: `tests/integration/mailbox/test_dead_letter_queue.cpp`
- Create: `tests/system/test_dlq_handoff.cpp`

- [ ] **Step 1: Add DLQ handoff integration tests**

In `tests/integration/mailbox/test_dead_letter_queue.cpp`, add after the existing test:

```cpp
TEST(DeadLetterQueueIntegrationTest, RecordStoresPayloadAndTrace) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    TraceContext tc;
    tc.trace_id_hi = 0xABCD;
    tc.trace_id_lo = 0x1234;
    tc.span_id = 0x5678;

    DeadLetterRecord dl;
    dl.reason = DeadLetterReason::OverflowPolicy;
    dl.source = DeadLetterSource::MailboxAdmission;
    dl.message_id = 42;
    dl.timestamp_ns = 1000;
    dl.trace_id_hi = tc.trace_id_hi;
    dl.trace_id_lo = tc.trace_id_lo;
    dl.span_id = tc.span_id;
    dl.payload_sample = StreamBuffer{0xDE, 0xAD, 0xBE, 0xEF};
    dl.payload_size = 4;

    EXPECT_TRUE(q.try_push(std::move(dl)));

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.reason, DeadLetterReason::OverflowPolicy);
    EXPECT_EQ(out.message_id, 42u);
    EXPECT_EQ(out.timestamp_ns, 1000u);
    EXPECT_EQ(out.trace_id_hi, 0xABCDu);
    EXPECT_EQ(out.trace_id_lo, 0x1234u);
    EXPECT_EQ(out.span_id, 0x5678u);
    EXPECT_EQ(out.payload_size, 4u);
    EXPECT_EQ(out.payload_sample.size(), 4u);
    EXPECT_EQ(out.payload_sample[0], 0xDE);
    EXPECT_EQ(out.payload_sample[3], 0xEF);
}

TEST(DeadLetterQueueIntegrationTest, ConfigDisabledSkipsPush) {
    DeadLetterConfig cfg;
    cfg.enabled = false;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord dl;
    dl.reason = DeadLetterReason::OverflowPolicy;
    dl.message_id = 1;
    EXPECT_FALSE(q.try_push(std::move(dl)));

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 0u);
}

TEST(DeadLetterQueueIntegrationTest, ReplayPopsAndReturnsRecord) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord a; a.message_id = 1; q.try_push(std::move(a));
    DeadLetterRecord b; b.message_id = 2; q.try_push(std::move(b));
    DeadLetterRecord c; c.message_id = 3; q.try_push(std::move(c));

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop_at(1, out));
    EXPECT_EQ(out.message_id, 2u);

    // Verify only index 1 was removed
    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 2u);
    EXPECT_EQ(snap.total_popped, 1u);

    DeadLetterRecord r0, r1;
    EXPECT_TRUE(q.try_pop(r0));
    EXPECT_EQ(r0.message_id, 1u);
    EXPECT_TRUE(q.try_pop(r1));
    EXPECT_EQ(r1.message_id, 3u);
}

TEST(DeadLetterQueueIntegrationTest, ReplayNoPayloadFails) {
    DeadLetterConfig cfg;
    cfg.capacity = 10;
    DeadLetterQueue q(cfg);

    DeadLetterRecord a;
    a.reason = DeadLetterReason::OverflowPolicy;
    a.message_id = 1;
    a.payload_sample.clear();
    q.try_push(std::move(a));

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_TRUE(out.payload_sample.empty());
}

TEST(DeadLetterQueueIntegrationTest, OverflowPreservesOldestWhenFull) {
    DeadLetterConfig cfg;
    cfg.capacity = 3;
    cfg.overflow_policy = DeadLetterOverflowPolicy::DropOldestRecord;
    DeadLetterQueue q(cfg);

    for (uint64_t i = 0; i < 5; ++i) {
        DeadLetterRecord r; r.message_id = i;
        q.try_push(std::move(r));
    }

    auto snap = q.snapshot();
    EXPECT_EQ(snap.depth, 3u);
    EXPECT_EQ(snap.total_pushed, 5u);
    EXPECT_EQ(snap.total_lost, 2u);

    DeadLetterRecord out;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.message_id, 2u); // 0,1 dropped
}
```

- [ ] **Step 2: Add end-to-end system test for DLQ handoff via ActorSystem**

Create `tests/system/test_dlq_handoff.cpp`:

```cpp
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

// Verify dead_letter_queue() accessor works and DLQ is created.
TEST(DlqHandoffSystemTest, DeadLetterQueueAccessorWorks) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.enable_cli = false;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.capacity = 100;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);
    EXPECT_TRUE(dlq->config().enabled);

    // Push a record through the public API
    mailbox::DeadLetterRecord dl;
    dl.reason = mailbox::DeadLetterReason::ActorNotFound;
    dl.source = mailbox::DeadLetterSource::LocalDelivery;
    dl.message_id = 42;
    EXPECT_TRUE(system.dead_letter(std::move(dl)));

    auto snap = system.dead_letter_snapshot();
    EXPECT_EQ(snap.depth, 1u);

    mailbox::DeadLetterRecord out;
    EXPECT_TRUE(system.pop_dead_letter(out));
    EXPECT_EQ(out.message_id, 42u);
}

// Verify disabled DLQ does not accept records.
TEST(DlqHandoffSystemTest, DisabledDlqRejectsRecords) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.enable_cli = false;
    cfg.dead_letters.enabled = false;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);
    EXPECT_FALSE(dlq->config().enabled);

    mailbox::DeadLetterRecord dl;
    dl.message_id = 1;
    EXPECT_FALSE(system.dead_letter(std::move(dl)));

    auto snap = system.dead_letter_snapshot();
    EXPECT_EQ(snap.depth, 0u);
}

// Verify snapshot_records via system path.
TEST(DlqHandoffSystemTest, SnapshotRecordsViaSystem) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.enable_cli = false;
    cfg.dead_letters.capacity = 50;

    ActorSystem system(cfg);
    auto* dlq = system.dead_letter_queue();

    for (uint64_t i = 0; i < 3; ++i) {
        mailbox::DeadLetterRecord r;
        r.message_id = i;
        dlq->try_push(std::move(r));
    }

    auto records = dlq->snapshot_records();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].message_id, 0u);
    EXPECT_EQ(records[2].message_id, 2u);

    // Records are still in queue (snapshot doesn't remove)
    auto snap = dlq->snapshot();
    EXPECT_EQ(snap.depth, 3u);
}
```

- [ ] **Step 3: Build and run all tests**

```bash
ninja -C build
ctest -R "dead_letter|dlq_handoff|DeadLetter" --output-on-failure -j8
```

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/mailbox/test_dead_letter_queue.cpp \
        tests/system/test_dlq_handoff.cpp
git commit -m "test(mailbox): add DLQ handoff integration and system tests

Covers payload/trace preservation, config gating, replay via try_pop_at,
overflow eviction, and end-to-end ActorSystem DLQ access."
```

---

### Task 9: Full verification

- [ ] **Step 1: Full build**

```bash
ninja -C build
```

- [ ] **Step 2: Run full test suite**

```bash
ctest --output-on-failure --parallel 8
```

- [ ] **Step 3: Run existing DLQ and mailbox tests to confirm no regressions**

```bash
ctest -R "mailbox|dead_letter" --output-on-failure -j8
```

- [ ] **Step 4: Check git log**

```bash
git log --oneline -10
```

Expected: 8 commits on branch `worktree-mbx-004-dlq-handoff-design`.
