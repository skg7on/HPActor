# MBX-001: Bounded Mailbox Capacity — Design Spec

**Issue**: [#22](https://github.com/skg7on/HPActor/issues/22)
**Subsystem**: Mailbox
**Priority**: P0
**Status**: Partially implemented — message-count bounding done, byte budget missing

## 1. Current State Audit

### 1.1 What's Already Implemented

| Requirement | Status | Evidence |
|---|---|---|
| Per-actor & system default capacity at spawn | Done | `mailbox_config_for_spawn()` / `mailbox_config_for_actor_def()` in `actor_system.cpp:371-395`; TOML `[system.mailbox]` parsed by `mailbox_config_parser.cpp` |
| Enqueue returns a result | Done | `try_push()` returns `EnqueueResult` with code, depth, capacity, pressure_ratio |
| Capacity observable | Done | `snapshot()` returns depth, capacity, queued_bytes, max_depth, counters, pressure_state; metrics ring buffer events on enqueue/dequeue/reject/drop/DLQ |
| Depth observable | Done | `snapshot().depth`, `snapshot().max_depth`, `snapshot().total_enqueued`, `snapshot().total_dequeued` |
| Message-count admission control | Done | CAS-based `try_reserve()` in `mpsc_actor_mailbox.hpp:339-355` enforces `max_messages` |
| Overflow policies (RejectNewest, DropNewest, DropOldest, DeadLetter) | Done | `try_push()` switch in `mpsc_actor_mailbox.hpp:84-153` |
| System message protected reserve | Done | `try_reserve_system()` in `mpsc_actor_mailbox.hpp:367-384`, default 32 slots |
| Soft-pressure watermark signalling | Done | `pressure_code_after_accept()` returns `AcceptedWithSoftPressure` when depth/capacity >= high_watermark |
| Dead letter handoff on overflow | Done | `try_deliver_local()` in `actor_system.cpp:531-548` captures DL record on rejection |
| High-depth warning log | Done | Warning when depth > 1024 (`mpsc_actor_mailbox.hpp:204-210`) |

### 1.2 What's NOT Implemented (MBX-001 Gap)

**Byte budget enforcement.** The `MailboxCapacity::max_bytes` field exists (default 0 = unlimited) and flows through config → `MailboxConfig` → `MPSCActorMailbox`, but `try_reserve()` ignores its `bytes` parameter:

```cpp
// mpsc_actor_mailbox.hpp:339
bool try_reserve(uint64_t /*bytes*/) noexcept {
    uint32_t cap = config_.capacity.max_messages;
    // ... only checks max_messages, never max_bytes
}
```

`queued_bytes_` is tracked (incremented on enqueue, decremented on dequeue) but never used for admission gating. The `estimate_message_bytes()` free function and `MailboxEnvelopeMeta::estimated_bytes` plumbing are all in place — the only missing piece is the admission check itself.

### 1.3 Design Debt / Half-Implementation Notes

These are not part of MBX-001 but are worth noting for context:

- **Overflow policies not yet implemented**: `DropLowestPriority`, `SpillToOverflowQueue`, `SignalOnly`, `BlockWhenAllowed` fall through to the `default:` case which returns `Rejected`. No tests exist for these paths.
- **Priority-aware lanes**: `MailboxConfig::priority_aware` and `priority_levels` are defined but `MPSCActorMailbox` uses a single FIFO queue regardless.
- **Backpressure signal propagation**: `BackpressureMode`, `BackpressureSignal`, `BackpressureReason` types exist, and `MailboxPressureState` has `HardPressure`/`Recovering` variants, but only `Normal`/`SoftPressure` are ever set. The `update_pressure_state()` only toggles between Normal and SoftPressure based on `high_watermark`.

## 2. Design: Byte Budget Enforcement

### 2.1 Admission Model

The current CAS-based reservation loop is the correct pattern. We extend it to check both dimensions (count AND bytes) in a single combined reservation:

```
try_reserve(bytes):
  cap_msgs = config_.capacity.max_messages
  cap_bytes = config_.capacity.max_bytes

  if cap_msgs == 0 and cap_bytes == 0:
    return true  // unlimited both dimensions

  loop:
    current_msgs = reserved_messages_
    current_bytes = queued_bytes_

    if cap_msgs > 0 and current_msgs >= cap_msgs:
      return false
    if cap_bytes > 0 and current_bytes + bytes > cap_bytes:
      return false

    // CAS both counters together — two 32-bit values packed into 64-bit
    desired_msgs = current_msgs + 1
    desired_bytes = current_bytes + bytes
    desired = (desired_bytes << 32) | desired_msgs
    current_packed = (current_bytes << 32) | current_msgs

    if CAS(&reserved_state_, current_packed, desired):
      return true
```

**Rationale for packed CAS**: Currently `reserved_messages_` and `queued_bytes_` are separate atomics, which means a producer could pass the count check, then another producer sneaks in and pushes bytes over the budget before the first producer atomically commits its byte increment. A single 64-bit CAS on a packed (msgs, bytes) word prevents this TOCTOU race.

**Alternative — two-phase with separate atomics**: Accept the TOCTOU window and treat it as "best-effort" byte budgeting. Pro: simpler, no packed CAS. Con: can overshoot byte budget by (N_producers × avg_msg_bytes). Rejected because the whole point of byte budgeting is OOM prevention, and a burst of concurrent producers on a large-payload workload could substantially overshoot.

**Alternative — mutex**: Rejected because the hot path must remain lock-free for cooperative scheduling guarantees.

### 2.2 Release on Dequeue

Dequeue must release from the same packed counter. Currently `release_reservation(bytes)` does two separate `fetch_sub` calls. We replace with a single `fetch_sub` on the packed word:

```
release_reservation(bytes):
  reserved_state_.fetch_sub((bytes << 32) | 1)
```

### 2.3 System Message Protected Reserve

System messages that pass through `try_reserve_system()` currently bypass byte tracking entirely (`release_system_reservation` checks `max_bytes > 0` but `try_reserve_system` doesn't). We keep this asymmetry — system messages are small control-plane messages, and the protected reserve is count-capped (default 32). Tracking their bytes would add complexity without meaningful OOM protection benefit since the count cap already bounds their memory footprint.

However, we should still increment `queued_bytes_` for system messages (which we already do — `enqueue_reserved` unconditionally adds to `queued_bytes_`), so `snapshot().queued_bytes` remains accurate.

### 2.4 DropOldest Interaction

`drop_one_oldest()` already calls `release_reservation(bytes)` — no change needed; the packed-counter release handles it correctly.

### 2.5 Config Defaults

`max_bytes = 0` means "unlimited" — this preserves backward compatibility. The TOML config accepts `default_byte_capacity` (currently read as `uint32_t`, should be `uint64_t`). When both `max_messages` and `max_bytes` are 0, the mailbox is fully unbounded (current behavior for legacy users).

### 2.6 Byte Estimation

The existing `estimate_message_bytes()` returns `sizeof(TypedMessage) + payload.size()`. This is a lower bound (doesn't account for allocator metadata, envelope, or queue node overhead). For OOM prevention, a lower bound is acceptable — the goal is to bound the dominant memory consumer (payload data), not to account every byte. If precise accounting is needed later, we can add a configurable overhead factor.

## 3. Implementation Plan

### Step 1: Replace split atomics with packed 64-bit CAS

**File**: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

- Replace `reserved_messages_` (uint32_t) and `queued_bytes_` (uint64_t) with a single `std::atomic<uint64_t> reserved_state_` packing (msgs:u32, bytes_hi:u32). Wait — queued_bytes_ is uint64_t, so we can't pack both into 64 bits. We need a different approach.

**Revised approach — keep split atomics with a careful ordering protocol:**

Since `queued_bytes_` is uint64_t and we need both count (32-bit) and bytes (64-bit), packing into 64 bits doesn't work. Instead:

1. Keep `reserved_messages_` (uint32_t) and `queued_bytes_` (uint64_t) as separate atomics
2. In `try_reserve(bytes)`:
   - First CAS `reserved_messages_` to claim a count slot
   - Then do a `fetch_add` on `queued_bytes_` and check if the result exceeds `max_bytes`
   - If it does, roll back: `fetch_sub` on `reserved_messages_` and `queued_bytes_`, return false
   - Use `std::memory_order_acquire` on the `fetch_add` to ensure visibility

This has a TOCTOU race but avoids the complexity of a 128-bit CAS (not lock-free on x86_64). The worst case is (N_producers - 1) × max_msg_bytes of overshoot. For most workloads this is bounded to well under 1 MB and acceptable for OOM prevention.

Actually, let me reconsider. The simplest correct approach:

**Final approach — check-then-increment with retry loop on bytes:**

```cpp
bool try_reserve(uint64_t bytes) noexcept {
    uint32_t cap = config_.capacity.max_messages;
    uint64_t byte_cap = config_.capacity.max_bytes;

    // Step 1: CAS the message count
    if (cap > 0) {
        uint32_t current = reserved_messages_.load(std::memory_order_acquire);
        while (true) {
            if (current >= cap) return false;
            if (reserved_messages_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break; // count reserved
            }
        }
    }

    // Step 2: Try to reserve bytes
    if (byte_cap > 0) {
        uint64_t current = queued_bytes_.load(std::memory_order_acquire);
        while (true) {
            if (current + bytes > byte_cap) {
                // Roll back count reservation
                if (cap > 0) {
                    reserved_messages_.fetch_sub(1, std::memory_order_release);
                }
                return false;
            }
            if (queued_bytes_.compare_exchange_weak(
                    current, current + bytes, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break; // bytes reserved
            }
        }
    } else if (cap == 0) {
        // Unlimited count, but still track bytes for observability
        if (byte_cap == 0) {
            queued_bytes_.fetch_add(bytes, std::memory_order_release);
        }
    }

    return true;
}
```

Wait, this is getting complex. Let me simplify. The existing code already has `reserved_messages_` and `queued_bytes_` as separate atomics, and the current `try_reserve` only checks `reserved_messages_`. The simplest change that closes the gap:

1. After the count CAS succeeds, do a bytes check
2. If bytes would exceed budget, release the count reservation and return false

This preserves the existing structure with minimal changes and acceptable TOCTOU window.

But wait, there's actually a simpler approach. Currently `try_reserve` ignores bytes entirely. The cleanest fix:

**Keep the existing `reserved_messages_` CAS, then add a `queued_bytes_` CAS in the same function.** Two-phase reservation with rollback on byte-budget failure. The TOCTOU window is (N-1) * max_msg_size which is bounded and acceptable.

### Step 2: Update `release_reservation` to match

No structural change needed — `release_reservation` already subtracts from both counters. Just remove the conditional on `max_bytes` from `release_system_reservation` (since `queued_bytes_` is always tracked).

### Step 3: Update snapshot and metrics

- `snapshot().queued_bytes` — already correct
- `snapshot().byte_capacity` — already correct
- No new metrics needed; the existing rejection counter captures byte-budget rejections

### Step 4: Tests

Add test cases to `test_bounded_mailbox.cpp`:

1. **Byte budget enforcement**: Set `max_bytes = sizeof(TypedMessage) + 10`, send messages with 5-byte and 6-byte payloads, verify second message is rejected
2. **Combined count + byte budget**: Set both limits, verify whichever is hit first triggers rejection
3. **Unlimited bytes (max_bytes=0)**: Verify existing count-only behavior preserved
4. **Byte budget with system messages**: System messages should still pass through the protected reserve even when byte budget is exhausted (since `try_reserve_system` doesn't check bytes)
5. **Snapshot byte counters**: Verify `queued_bytes` and `byte_capacity` in snapshot after enqueue/dequeue cycle
6. **DropOldest frees bytes**: Verify that `DropOldest` policy frees the byte budget correctly for the replacement message

### Step 5: Config fix

In `mailbox_config_parser.cpp:76`, `default_byte_capacity` is read as `uint32_t` but the field is `uint64_t`. Fix to `read_uint64`.

## 4. Files Changed

| File | Change |
|---|---|
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Add byte-budget check to `try_reserve()` |
| `src/config/parsers/mailbox_config_parser.cpp` | Fix `read_uint32` → `read_uint64` for byte capacity |
| `tests/unit/mailbox/test_bounded_mailbox.cpp` | Add byte-budget test cases |
| `tests/unit/mailbox/test_mailbox_overflow_policies.cpp` | Add byte-budget + DropOldest interaction test |

## 5. Risk Assessment

- **Performance**: Adds a second CAS loop to the hot path only when `max_bytes > 0`. When `max_bytes == 0` (default), the fast path is unchanged.
- **TOCTOU overshoot**: At most (N_concurrent_producers - 1) × max_single_msg_bytes. Acceptable for OOM prevention — the goal is to bound memory, not enforce an exact quota.
- **Backward compatibility**: `max_bytes = 0` preserves unlimited behavior. Existing code that only sets `max_messages` is unaffected.
- **No new allocations, no exceptions, no RTTI**: The change is purely arithmetic on existing atomics.

## 6. Out of Scope (Deferred to Other MBX Tickets)

- Priority-aware multi-lane queue (MBX-005)
- Remote backpressure signal propagation (MBX-003)
- DropLowestPriority / SpillToOverflowQueue / SignalOnly / BlockWhenAllowed overflow policies (MBX-002)
- HardPressure / Recovering pressure states with hysteresis (MBX-003)
- Remote outbound queue limits (MBX-006)
