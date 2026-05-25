# MBX-001: Byte Budget Enforcement — Implementation Plan

## Context

MBX-001 asks for bounded mailbox capacity by message count AND byte budget.
Message-count bounding is already done. The gap: `try_reserve()` ignores its
`bytes` parameter — `max_bytes` flows through config but is never enforced.
This plan closes that gap by adding a byte-budget CAS loop after the existing
count reservation in `try_reserve()`.

## Files Changed

| File | Change |
|---|---|
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | Add byte-budget CAS to `try_reserve`; fix `queued_bytes_` ownership across reserve/release/inject/test paths |
| `src/config/parsers/mailbox_config_parser.cpp` | Fix `default_byte_capacity` from `uint32_t` to `uint64_t` |
| `tests/unit/mailbox/test_bounded_mailbox.cpp` | Add 5 byte-budget test cases |
| `tests/unit/mailbox/test_mailbox_overflow_policies.cpp` | Add DropOldest + byte budget interaction test |

## Implementation Steps

### Step 1: Add byte-budget CAS to `try_reserve`

Two-phase reservation: count CAS first (unchanged logic), then byte-budget
CAS with rollback on failure.

When `max_bytes == 0` (default), byte budget is unlimited — single `fetch_add`
for observability only. Zero overhead on the existing count-only path.

### Step 2: Fix `queued_bytes_` ownership

`enqueue_reserved` was doing `queued_bytes_.fetch_add` — but both `try_reserve`
and `try_reserve_system` also add to `queued_bytes_`. This caused
double-counting. Remove the `fetch_add` from `enqueue_reserved` and
consolidate byte tracking in the reservation methods.

- `try_reserve(bytes)` — CAS-increments `queued_bytes_` if `max_bytes > 0`;
  `fetch_add` if unlimited bytes
- `try_reserve_system(bytes)` — `fetch_add` on `queued_bytes_` for
  observability (system messages bypass byte budget but still track bytes)
- `enqueue_reserved` — remove `queued_bytes_.fetch_add`
- `release_reservation(bytes)` — unchanged
- `release_system_reservation(bytes)` — remove stale `max_bytes > 0` conditional
- `inject_for_test` — add `queued_bytes_.fetch_add`

### Step 3: Config parser fix

Change `read_uint32("default_byte_capacity", 0)` to read via
`value().as_int64()` cast to `uint64_t`. No `read_uint64` exists on
`TomlTableView`, so use the `TomlValueView` path.

### Step 4: Tests

**test_bounded_mailbox.cpp** — new `ByteBudgetTest` fixture:
- `ByteBudgetRejectsWhenExceeded` — byte budget gates admission
- `CombinedCountAndByteBudget` — count limit hits before byte limit
- `UnlimitedBytesPreservesCountOnly` — `max_bytes=0` behaves as before
- `DequeueReleasesBytes` — freed bytes allow re-enqueue
- `SnapshotReflectsByteBudget` — `queued_bytes`/`byte_capacity` in snapshot

**test_mailbox_overflow_policies.cpp**:
- `DropOldestFreesByteBudget` — DropOldest frees enough bytes for the
  replacement message

## Verification

```bash
cmake -S . -B build -GNinja && ninja -C build
./build/tests/unit/mailbox/test_unit_mailbox
./build/tests/unit/mailbox/test_mailbox_overflow
ctest --output-on-failure --parallel 8
```
