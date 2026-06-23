# Per-Exception Supervision Nesting — Design Document

## 1. Executive Summary

Akka supports per-exception-type supervision directives where different failure types map to different actions (e.g., `NullPointerException` → Restart, `IllegalStateException` → Stop). HPActor's supervision previously used a single strategy per supervisor. This design adds per-failure-reason directive overrides to `SupervisionPolicy`.

## 2. API

```cpp
struct SupervisionPolicy {
    // ... existing fields ...
    std::unordered_map<uint32_t, SupervisionDirective> exception_map;
};
```

Each entry maps a failure reason code (from `error` or `FailureReason`) to a specific `SupervisionDirective`. The supervisor checks this map before falling back to the default strategy.

## 3. Integration

In `SelfSupervisingActor::on_failure()`:
```cpp
auto it = policy_.exception_map.find(err.code());
if (it != policy_.exception_map.end()) return it->second;
return decide_restart(child_id, err);
```

## 4. Backward Compatibility

Empty `exception_map` preserves existing behavior — all failures use the default strategy.

## 5. Example

```cpp
policy.exception_map = {
    {FailureReason::MailboxFull, SupervisionDirective::Restart},
    {FailureReason::ActorDead,   SupervisionDirective::Stop},
    {FailureReason::Timeout,     SupervisionDirective::Escalate},
    {FailureReason::Quarantined, SupervisionDirective::Quarantine},
};
```

## 6. References
- [Supervision header](../../include/hpactor/supervision/supervision.hpp)
- [Akka Gap Analysis (Issue #329)](https://github.com/skg7on/HPActor/issues/329) — Supervision gap
