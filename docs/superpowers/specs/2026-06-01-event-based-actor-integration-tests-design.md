# EventBasedActor Integration Tests — Design Spec

**Date:** 2026-06-01
**Status:** Approved
**Scope:** Comprehensive integration tests for `EventBasedActor::on<T>()`, `on_request<ReqT,ResT>()`, `become()`, `receive()` pipeline, error paths, system message interception, lifecycle gate, and CLI dispatch.

## Background

`tests/integration/actor/test_event_based_actor.cpp` contains a single trivial static_assert.
The `on<T>()` and `on_request<ReqT,ResT>()` template methods have zero test coverage across
the entire test suite. The `receive()` pipeline (proto handler dispatch, behavior fallback,
system message interception, lifecycle gate, CLI dispatch) is exercised only indirectly
through other test files.

## Design

### Test structure

A single GTest fixture `EventBasedActorTest` extending `::testing::Test`:

```
SetUp:   Config{scheduler_threads=0, enable_network=false, cli.enabled=false, tracing.enabled=false}
         ActorSystem(system_) and test actor spawned
TearDown: system_->shutdown()
```

All tests use `scheduler_threads=0` for determinism. Messages are injected directly into the
mailbox, then `receive()` is called synchronously. No timing assumptions, no thread races.

### Test actor

A dedicated `TestEventHandler` actor subclassed from `EventBasedActor` with:

- `int on_count_` — incremented by fire-and-forget handlers
- `int request_count_` — incremented by request-response handlers
- `MetricsRequest last_request_` — captured last request for inspection
- `bool become_called_` / `int become_chain_step_` — become() tracking
- Override `register_handlers()` to register test-specific handlers dynamically
- Public `register_handler_for_test<T>()` / `register_request_handler_for_test<ReqT,ResT>()` for per-test handler setup

### Protobuf types

Use existing system protobuf types with `MessageTraits` specializations:
- `MetricsRequest` (TypeTag::MetricsRequestTag = 0x40) — simple empty message
- `MetricsResponse` (TypeTag::MetricsResponseTag = 0x41) — bytes body
- `DownMessage` (TypeTag::DownMsg = 0x01) — has actor_id, reason_code
- `ExitMessage` (TypeTag::ExitMsg = 0x02)

### Message injection pattern

```cpp
auto* mailbox = system_->get_mailbox(actor.address().id);
TypedMessage msg(MetricsRequestTag, serialized_payload);
msg.set_sender_address(ActorAddress{}); // anonymous sender
mailbox->inject_for_test(msg);
actor.receive(msg); // synchronous dispatch
```

### Test cases

#### Section 1: Proto handler registration & dispatch (8 tests)

| Test | Verifies |
|------|----------|
| `OnInvokesHandlerForMatchingTag` | `on<T>()` registered handler fires when matching TypeTag arrives |
| `OnDoesNotFireForDifferentTag` | Handler registered for tag A is not invoked for tag B |
| `OnRequestSerializesAndReplies` | `on_request<ReqT,ResT>()` handler response is serialized and sent via `context()->reply()` |
| `OnRequestNoReplyWhenResponseEmpty` | Empty response → no reply enqueued |
| `HandlesReturnsTrueForRegisteredTag` | `handles(tag)` returns true after `on<T>(tag)` |
| `HandlesReturnsFalseForUnregisteredTag` | `handles(tag)` returns false for unknown tag |
| `HandlersInitializedLazily` | `register_handlers()` not called until first `receive()` |
| `MultipleHandlersForDifferentTags` | Two `on<T>()` registrations, both invoked for their respective tags |

#### Section 2: become / become_empty (4 tests)

| Test | Verifies |
|------|----------|
| `BecomeReplacesCurrentBehavior` | After `become(B)`, messages dispatched to B |
| `BecomeEmptyDropsMessages` | After `become_empty()`, behavior is a no-op |
| `BecomeFromWithinHandler` | Handler calls `become()` during receive; next message uses new behavior |
| `RepeatedBecomeCycle` | Chain A→B→C→empty, one message per stage, correct dispatch verified |

#### Section 3: receive() dispatch priority (3 tests)

| Test | Verifies |
|------|----------|
| `ProtoHandlerPriorityOverBehavior` | When both registered, proto handler fires, not behavior |
| `BehaviorFallbackForUnknownTag` | Unknown tag falls through to behavior |
| `NoOpForUnknownTagAndEmptyBehavior` | No handler + empty behavior = no crash, no side effects |

#### Section 4: Error paths & edge cases (4 tests)

| Test | Verifies |
|------|----------|
| `DeserializationFailureIsSafe` | Corrupted payload → deserialize returns null → handler not invoked, no crash |
| `UnknownTypeTagNoSideEffects` | Unknown tag + no behavior → graceful no-op |
| `TwoConsecutiveMessagesBothHandled` | Two messages with same tag → handler invoked twice |
| `EmptyPayloadSafe` | Zero-length payload → deserialization fails gracefully |

#### Section 5: System message interception (3 tests)

| Test | Verifies |
|------|----------|
| `LinkMsgIntercepted` | LinkMsg (0x03) intercepted by `handle_link_msg`, not passed to proto handler even if registered |
| `DownMsgCleansUpLinkedMonitored` | DownMsg removes sender from linked/monitored lists |
| `MonitorMsgRegistration` | MonitorMsg (0x0A) adds sender to monitored actors |

#### Section 6: Lifecycle gate (2 tests)

| Test | Verifies |
|------|----------|
| `QuarantinedActorRejectsUserMessages` | When lifecycle state is quarantined, user tags (≥0x1000) rejected |
| `ActiveActorAcceptsUserMessages` | In Active lifecycle state, user messages reach proto handlers |

#### Section 7: CLI dispatch (2 tests)

| Test | Verifies |
|------|----------|
| `InspectStateRequestReturnsMetadata` | InspectStateRequest → reply with actor metadata |
| `KillRequestDrivesLifecycleToStopped` | KillRequest → lifecycle Stopping→Stopped transition |

**Total: 26 tests**

### Test design constraints (from CLAUDE.md)

- `scheduler_threads = 0` — no scheduler races
- `inject_for_test()` — place messages without scheduler notification
- No timing assumptions, no sleeps
- No platform-specific assertions
- No reliance on NDEBUG-compiled-out asserts
- Existing patterns followed: `UnifiedMessagePassingTest` fixture, `minimal_config()` from `system_test_fixture.hpp`

### File changes

- **Modified:** `tests/integration/actor/test_event_based_actor.cpp` — replace trivial test with 26 comprehensive tests
- No new files, no CMakeLists.txt changes needed (test binary already exists and is linked with hpactor, hpactor_proto, GTest)
