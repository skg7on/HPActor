# Python Actor Programming Interface — Native Integration Design

**Status:** Approved design
**Date:** 2026-07-10
**Target:** Wire the `hpactor/` Python package to the native `_hpactor` module (pybind11 backend)
**Depends on:** pybind11 backend (2026-07-10-pybind11-backend-design.md)

## 1. Summary

The `hpactor/` Python package currently uses a self-contained fake/stub system
for unit testing. The `ActorSystem`, `Actor`, `Behavior`, and `ActorContext` APIs
are fully defined but not connected to the native `_hpactor` module. This design
wires the Python actor programming interface to the real native bridge while
preserving the fake system as the default for fast unit tests.

## 2. Architecture

### 2.1 Dual-Mode Design

Two execution modes share the same public API, selected by factory injection:

```
                    ┌──────────────────────────────┐
                    │      ActorSystem (public)      │
                    │  spawn / send / ask / close    │
                    └──────────────┬───────────────┘
                                   │
                    ┌──────────────▼───────────────┐
                    │    _NativeProtocol (new)       │
                    │  start / stop / spawn_bridge   │
                    │  submit / drain_dispatch       │
                    │  drain_completions / snapshot  │
                    │  dispatch_fd / completion_fd   │
                    └──────┬──────────┬─────────────┘
                           │          │
              ┌────────────▼──┐  ┌───▼──────────────┐
              │  FakeSystem   │  │  NativeSystem     │
              │  (default)    │  │  (_hpactor)       │
              │  in-process   │  │  real bridge      │
              └───────────────┘  └──────────────────┘
```

**`_NativeProtocol`** is an internal duck-typed protocol defining the interface
both backends implement:

- `start()`, `stop()`, `begin_draining()`
- `spawn_bridge() → (address_tuple, generation)`
- `stop_bridge(address_tuple) → bool`
- `register_name(name, address_tuple) → bool`
- `resolve_name(name) → address_tuple | None`
- `submit(command_dict) → bool`
- `drain_dispatch(max_items) → list[dict]`
- `drain_completions(max_items) → list[dict]`
- `snapshot() → dict`
- `dispatch_fd: int`, `completion_fd: int`
- `application_origin() → address_tuple`

**`FakeNativeSystem`** (default, `use_native=False`): uses the current
in-process `_DispatchCoordinator`, `_TokenRegistry`, and synthetic addresses.
Zero changes to existing behavior. All 22 existing unit tests pass unchanged.

**`Pybind11NativeSystem`** (opt-in, `use_native=True`): wraps
`_hpactor.NativeSystem` and delegates all protocol methods to the native module.

### 2.2 Public API

`ActorSystem.__init__` gains one keyword-only parameter:

```python
def __init__(
    self,
    messages: MessageRegistry,
    *,
    config: Optional[Dict[str, Any]] = None,
    use_native: bool = False,  # NEW
):
```

When `use_native=False` (default), the fake system is used. When `True`,
`_hpactor` is imported and the real bridge is wired up. `use_native=True`
without an importable `_hpactor` raises `NativeBindingUnavailable`.

## 3. Real-Mode Runtime Loop

When `use_native=True`, `_RuntimeThread` operates in native mode:

### 3.1 Startup

1. Import `_hpactor` lazily (only on the runtime thread)
2. Construct `_hpactor.NativeSystem(config_dict)` — creates C++ runtime, queues, notifiers
3. Call `native.start()` — starts `PythonRuntime`, gateway actor, application bridge
4. Register two `loop.add_reader()` callbacks on the dedicated asyncio event loop:

   - **`dispatch_fd` → `_on_dispatch_readable()`:**
     Drains `native.drain_dispatch(max_dispatch_per_tick)`, converts each dict to
     the coordinator's internal format, enqueues via `coordinator.enqueue()`,
     calls `coordinator.schedule_next()`.

   - **`completion_fd` → `_on_completion_readable()`:**
     Drains `native.drain_completions(max_completions_per_tick)`, resolves
     `_TokenRegistry` futures by token.

### 3.2 Runtime Components (reused from fake mode)

- **`_DispatchCoordinator`**: receives dispatch dicts from the fd reader
  instead of from `ActorSystem.send()` directly. Per-actor non-reentrant
  scheduling, FIFO ordering, and active-actor gating are unchanged.
- **`_ActorRunner`**: unchanged — still deserializes protobuf, invokes handlers,
  manages context expiration, auto-replies for request-response.
- **`_TokenRegistry`**: handles completion resolution by token (same as fake
  mode, but completions now arrive from the native fd reader instead of from
  in-process actor emulation).

### 3.3 Shutdown Sequence

1. `native.begin_draining()` — rejects new spawns and dispatches
2. Cancel active handlers with `handler_shutdown_timeout_ms`
3. Run `on_stop()` once on each Python actor (on the runtime loop)
4. Remove `loop.remove_reader(dispatch_fd)` and `loop.remove_reader(completion_fd)`
5. `native.stop()` — stops gateway, bridges, runtime; joins C++ threads
6. Stop the asyncio event loop
7. Join the runtime thread from the application thread

## 4. spawn() — Real Mode

`ActorSystem.spawn()` follows these sequential steps on the dedicated runtime loop:

1. **Marshal to runtime loop** — all spawn work runs on the dedicated asyncio thread
   via `_thread.submit()`.
2. **`native.spawn_bridge()`** — creates a native `PythonBridgeActor`, returns
   `(address_tuple, generation)`. The bridge is the real HPActor identity
   (mailbox, supervision, registry entry, lifecycle hooks).
3. **Construct Python actor** — `actor_class(*args, **kwargs)`, bind behavior
   via `instance._bind(registry)`.
4. **Create `_ActorRunner`** — keyed by `(actor_id, generation)` from the native address.
5. **Install runner** on the coordinator.
6. **`native.register_name(name, address)`** — publishes the name in the native
   name registry.
7. **`await instance.on_start()`** — lifecycle hook runs on the runtime loop.
   If `on_start()` fails, the bridge is stopped and the runner is removed before
   re-raising.
8. **Return `ActorRef`** — with the real native address and generation.

The bridge is not externally visible (no name registered, no `SystemInit`
delivered) until `on_start()` succeeds. This matches the Phase 1E topology
contract for pre-publication validation.

## 5. send() and Command Submission

### 5.1 send() — Real Mode

`ActorSystem.send()` in real mode:

1. Serialize the protobuf message via `registry.serialize(message)`
2. Allocate a delivery token from `_TokenRegistry`
3. Build the command dict:
   ```python
   command = {
       "kind": CommandKind.SEND,
       "token": token,
       "sequence": next_sequence,
       "generation": generation,
       "origin": application_origin,       # application bridge address
       "target": target_ref.address_tuple,
       "type_tag": registry.type_tag_for(type(message)),
       "payload": serialized_bytes,
   }
   ```
4. Call `native.submit(command)` — enqueues into the native command queue,
   signals the gateway wake port
5. Return `DeliveryReceipt` — resolved asynchronously when the completion fd
   reader drains the matching delivery result

### 5.2 Context Operations

All `ActorContext` methods (`reply`, `ask`, `spawn` child, `schedule`,
`cancel_schedule`, `link`, `unlink`, `monitor`, `demonitor`, `stop`,
`passivate`) follow the same pattern:

1. Serialize arguments into a command dict
2. Allocate a token (for operations that need a result)
3. Call `native.submit(command_dict)` 
4. Await the token future (for async operations) or return immediately

The command dict uses the same field names as `NativeSystemObject.dict_to_command()`.

### 5.3 Command Flow (end-to-end)

```
Python handler → context.send() → serialize → command dict
  → native.submit(command_dict) → command queue → signal gateway
  → PythonGatewayActor::receive() → forward F1 protected message
  → originating PythonBridgeActor::receive()
  → context->send(target, typed_message)  [executes on scheduler turn]
  → PythonCompletion → completion queue → signal completion_fd
  → asyncio reader → native.drain_completions()
  → _TokenRegistry.resolve(token, delivery_result)
```

## 6. Dispatch Flow (end-to-end)

```
C++ sender → DeliveryPipeline → admission → mailbox
  → PythonBridgeActor::receive() → PythonDispatchEnvelope
  → dispatch queue → signal dispatch_fd
  → asyncio reader: _on_dispatch_readable()
  → native.drain_dispatch(256) → list[dict]
  → for each dict: coordinator.enqueue(dispatch)
  → coordinator.schedule_next()
  → _ActorRunner.run_one(dispatch, runtime)
  → deserialize protobuf → invoke handler → auto-reply for request-response
```

### 6.1 Dispatch Dict Format

Native dispatch dicts use named keys (matching `NativeSystemObject.dispatch_to_dict()`):

```python
{
    "kind": int,           # 0=Message, 1=LinkedExit, 2=MonitorDown, 3=Restart
    "actor": tuple,        # (family, packed, port, type, id, incarnation)
    "generation": int,
    "type_tag": int,
    "payload": bytes,
    "sender": tuple,       # sender address
    "message_id": int,
    "ask_message_id": int,
    "priority": int,
    "deadline_ns": int,
    "flags": int,
    "ack_requested": bool,
    "sequence": int,
}
```

### 6.2 Completion Dict Format

```python
{
    "kind": int,           # CommandResult, AskResult, DeliveryResult, SpawnResult, etc.
    "token": int,
    "sequence": int,
    "failure_reason": int,
    "failure_source": int,
    "actor": tuple,
    "generation": int,
    "type_tag": int,
    "payload": bytes,
    "error_code": int,
    "detail": bytes,
    "schedule_handle": int,
    "delivery_status": int,
    "retry_after_ns": int,
}
```

## 7. Fake Mode (unchanged)

When `use_native=False` (default), `FakeNativeSystem` implements `_NativeProtocol`
using the current in-process machinery:

- `spawn_bridge()`: returns a synthetic `(address_tuple, 1)` where `actor_id`
  is derived from `id(instance)`
- `submit()`: enqueues directly into the coordinator's deque
- `drain_dispatch()`: drains from the coordinator's deque (simulates fd reading)
- `drain_completions()`: resolves tokens directly (simulates completion reading)
- `dispatch_fd` / `completion_fd`: return -1 (no real notifiers)
- `start()` / `stop()`: manage the `_RuntimeThread` lifecycle

All 22 existing Python unit tests pass unchanged. The fake system is the default
so existing test infrastructure continues to work.

## 8. Address Tuple Format

The address tuple format `(family, packed, port, actor_type, actor_id, incarnation)`
is shared between the fake and real systems. Both produce tuples via the same
conversion functions in `_address.py`.

| Field | Type | Description |
|-------|------|-------------|
| family | int | 0=local, 4=IPv4, 6=IPv6 |
| packed_address | bytes | 0, 4, or 16 bytes |
| port | int | Network port |
| actor_type | int | Actor type identifier |
| actor_id | int | Unique instance ID |
| incarnation | int | Monotonic generation counter |

## 9. Command Kind Enum

```python
class CommandKind(IntEnum):
    SEND = 0
    REPLY = 1
    REPLY_ERROR = 2
    ASK = 3
    SPAWN = 4
    SCHEDULE = 5
    CANCEL_SCHEDULE = 6
    LINK = 7
    UNLINK = 8
    MONITOR = 9
    DEMONITOR = 10
    STOP = 11
    PASSIVATE = 12
    ACTOR_FAILED = 13
    INSPECT = 14
    CANCEL_ASK = 15
```

## 10. Testing Strategy

### 10.1 Unit Tests (fake mode, unchanged)

All 22 existing tests in `bindings/python/tests/unit/` continue to pass with
`use_native=False` (the default). No test changes required.

### 10.2 Integration Tests (real mode, new)

New tests in `bindings/python/tests/integration/` that run with `use_native=True`:

- `test_native_spawn_and_echo.py` — spawn an actor, send a message, verify handler execution
- `test_native_ask_reply.py` — ask/response round-trip through the real bridge
- `test_native_delivery_receipt.py` — verify delivery receipt resolution
- `test_native_lifecycle.py` — `on_start`, `on_stop`, failed start rollback
- `test_native_shutdown.py` — clean shutdown with active actors
- `test_native_ordering.py` — per-actor message ordering through the real bridge

Tests use `asyncio.wait_for` only as deadlock guards. Real actor progress is
verified through future resolution and snapshot inspection.

### 10.3 Native Module Smoke Test

```python
async def test_native_module_smoke():
    import _hpactor
    ns = _hpactor.NativeSystem(config_dict)
    ns.start()
    addr, gen = ns.spawn_bridge()
    assert addr[4] > 0  # actor_id > 0
    ns.stop()
```

## 11. Non-Goals

This design does NOT:
- Change any native bridge, queue, actor, or runtime code
- Change the public Python API (`ActorSystem`, `Actor`, `Behavior`, etc.)
- Change the fake system's internal logic
- Add Phase 1E declarative topology wiring (that's a separate design)
- Add remote actor support (deferred)
- Make `use_native=True` the default (deferred until Phase 1D wheel packaging is complete)

## 12. Acceptance Criteria

1. `ActorSystem(use_native=True)` creates a real `_hpactor.NativeSystem` and starts it
2. `spawn()` creates a native bridge, Python actor, and returns an `ActorRef` with real address
3. `send()` submits commands through the native bridge and returns a `DeliveryReceipt`
4. Dispatch fd reader drains native dispatches and routes them to the correct actor runner
5. Completion fd reader drains native completions and resolves token futures
6. `ask()` round-trips through the native bridge with correct reply deserialization
7. `__aexit__` performs coordinated shutdown and joins the runtime thread
8. All 22 existing unit tests pass unchanged (fake mode, default)
9. New integration tests pass with real native bridge (real mode)
10. No Python.h, PyObject, or CPython API call outside `python_pybind11/`

## 13. References

- [Python language binding design](2026-07-03-python-language-binding-design.md)
- [pybind11 backend design](2026-07-10-pybind11-backend-design.md)
- [pybind11 backend implementation plan](../plans/2026-07-10-pybind11-backend-implementation.md)
- [Phase 1B actor API plan](../plans/2026-07-05-python-binding-phase1b-actor-api.md)
