# Cross-Actor Scheduling — Design Document

## 1. Executive Summary

The `schedule_to()` API (ACT-008) extends the existing self-delivery `schedule()` to allow any actor to schedule a message for delivery to any other actor. This closes an Akka parity gap where `context.scheduleOnce(delay, target, msg)` supports cross-actor targets but HPActor's `schedule()` was self-only.

## 2. API

```cpp
AlarmHandle schedule_to(const ActorAddress& target,
                         std::chrono::milliseconds delay, TypedMessage msg);
```

Same return type (`AlarmHandle`) and cancellation (`cancel_schedule()`) as `schedule()`.

## 3. Implementation

Reuses the existing `IScheduler::schedule_after()` infrastructure. The callback captures the target `ActorId` instead of `self_id`:

```cpp
auto callback = [sys, target_id, msg_ptr]() {
    sys->deliver_local(target_id, std::move(*msg_ptr));
};
```

## 4. Design Decisions

- **No scheduler changes needed.** `schedule_after()` already supports arbitrary callbacks.
- **Same AlarmHandle type.** Backward-compatible — `cancel_schedule()` works for both.
- **Target must be local.** Cross-node scheduling requires remote delivery (future).

## 5. References
- [Akka Gap Analysis (Issue #329)](https://github.com/skg7on/HPActor/issues/329) — Scheduling gap
- [ActorContext header](../../include/hpactor/actor/actor_context.hpp)
