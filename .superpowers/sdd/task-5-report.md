# Task 5 Report: Compose runtime lifecycle, actor leases, and snapshots

**Status:** Complete (GREEN)

## Files Created

| File | Purpose |
|------|---------|
| `bindings/python/native/include/hpactor/python/python_ports.hpp` | `GatewayWakePort` -- function pointer + void* context, no `std::function` |
| `bindings/python/native/include/hpactor/python/python_runtime.hpp` | `PythonRuntime`, `PythonActorLease`, `PythonRuntimeState` enum, `PythonRuntimeSnapshot` struct |
| `bindings/python/native/src/python_runtime.cpp` | Full runtime implementation: lifecycle state machine, actor lease tracking, queue delegation, stale completion rejection |
| `tests/unit/python/test_python_runtime.cpp` | Lifecycle, lease, and stale completion tests |

## Files Modified

| File | Change |
|------|--------|
| `bindings/python/native/CMakeLists.txt` | Added `src/python_runtime.cpp` to `hpactor_python_native` |
| `tests/unit/python/CMakeLists.txt` | Added `test_python_runtime.cpp` to `test_unit_python_binding` |

## Test Results

```
[  PASSED  ] 10 tests from 4 test suites:
  NativeNotifierTest (2): SignalAndDrainAreNonBlocking, MoveAndClosePreserveSingleOwnership
  PythonContractsTest (3): TagsUseReservedLocalRange, ConfigRejectsInvalidBounds, LanguageBindingFailureSourceIsAppendOnly
  PythonRuntimeQueuesTest (2): DispatchDrainHonorsBudgetAndOrder, FullQueueRetainsProducerOwnership
  PythonRuntimeTest (3): LifecycleIsExplicitAndStopIsIdempotent, ActorLeasesAreBoundedAndGenerational, RejectsCompletionForReplacedGeneration
```

## Design Summary

### PythonRuntimeState
Created -> Starting -> Running -> Draining -> Stopping -> Stopped (+ Failed terminal state)

### GatewayWakePort
Raw function pointer + void* context. No `std::function` -- avoids exception-throwing paths and heap allocations.

### PythonRuntime
- `create()` validates config, constructs queues
- `start()` accepts exactly one valid `GatewayWakePort`, only transitions Created->Starting->Running. Creates dispatch and completion notifiers. Notifier failure -> Failed.
- `begin_draining()` transitions Running->Draining
- `stop()` is idempotent, closes admission, closes notifiers, clears wake port, transitions to Stopped
- `reserve_actor()` under mutex, increments global monotonic generation, honors `max_actor_bindings`
- `try_push_dispatch/completion` check state == Running, signal corresponding notifier on success
- `try_push_command` checks state == Running, invokes wake port on success
- `try_push_completion` calls `generation_matches()` BEFORE signalling notifier; stale -> reject + increment counter

### PythonActorLease
- Move-only, reserved under mutex with global monotonic generation per reservation
- `bind(ActorId)` inserts id/generation pair
- Destructor erases only matching pair (guards against old lease erasing a replacement generation)
- `reset()` releases the binding early

### PythonRuntimeSnapshot
Contains `state`, `queues` (PythonQueueSnapshot), `actor_bindings`, `stale_completion_rejected`, `dispatch_notifier_fd`, `completion_notifier_fd`.

## Post-Completion Fixes

### Fix 1: Remove dead `transition_state` declaration (2026-07-07)

Removed the private method declaration `PythonRuntimeState transition_state(PythonRuntimeState target) noexcept;` from `python_runtime.hpp` line 306. This method was declared but never defined -- removing it prevents misleading readers and potential linker errors.

**Build:** `ninja -C build test_unit_python_binding` -- pass
**Tests:** `PythonRuntimeTest.*` -- 3/3 passed (LifecycleIsExplicitAndStopIsIdempotent, ActorLeasesAreBoundedAndGenerational, RejectsCompletionForReplacedGeneration)
**Commit:** `fix: remove dead transition_state declaration`
