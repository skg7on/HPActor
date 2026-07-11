# Python Native Integration — Implementation Plan

**Date:** 2026-07-10
**Status:** Plan
**Depends on:** pybind11 backend implemented, `_hpactor.NativeSystem` functional
**Design spec:** `docs/superpowers/specs/2026-07-10-python-native-integration-design.md`

## Goal

Wire the `hpactor/` Python package to the native `_hpactor` module via a
dual-mode `_NativeProtocol` seam. The fake system remains the default
(`use_native=False`); real mode (`use_native=True`) connects `ActorSystem`
to the native bridge for actual actor messaging through the C++ runtime.

## Architecture

```
ActorSystem(use_native=False|True)
  → _NativeProtocol (duck-typed internal interface)
    → FakeNativeSystem (default, unchanged)
    → Pybind11NativeSystem (new, wraps _hpactor.NativeSystem)
```

## Tech Stack

Python 3.11+, asyncio, protobuf, `_hpactor` (pybind11-based), HPActor native
bridge.

## Global Constraints

1. pybind11 backend must be implemented and passing before this work begins.
2. All 22 existing Python unit tests must pass unchanged (fake mode is default).
3. No changes to `_address.py`, `_errors.py`, `_delivery.py`, `_messages.py`,
   `_behavior.py`, `_actor.py`, or `_context.py`.
4. Only `_system.py`, `_runtime.py`, and `__init__.py` are modified; new files
   go under `bindings/python/hpactor/`.
5. `_hpactor` is imported lazily — never at package import time.
6. Scheduler workers never call Python, acquire the GIL, or wait for Python
   progress (enforced by the existing native bridge architecture).
7. Native dispatch and completion fds are pumped by `loop.add_reader()`
   callbacks on the dedicated asyncio thread.
8. Dict-based wire format for dispatch, completion, and command types (matching
   `NativeSystemObject` output).
9. Tests use `asyncio.wait_for` only as deadlock guards.
10. `Python.h` and `pybind11` headers remain confined to `python_pybind11/`.

## File Structure

### New files (5)

```
bindings/python/hpactor/_native_protocol.py    # _NativeProtocol + _NativeBackend enum
bindings/python/hpactor/_native_fake.py        # FakeNativeSystem (extracted from _runtime.py)
bindings/python/hpactor/_native_pybind11.py    # Pybind11NativeSystem
bindings/python/tests/integration/test_native_spawn_echo.py
bindings/python/tests/integration/test_native_ask_reply.py
```

### Modified files (4)

```
bindings/python/hpactor/_system.py             # +use_native param, dual-mode spawn/send/ask
bindings/python/hpactor/_runtime.py            # +native_mode support in _RuntimeThread
bindings/python/hpactor/__init__.py            # +NativeBindingUnavailable export
bindings/python/tests/integration/CMakeLists.txt (if needed for CTest registration)
```

---

## Task 1: Define _NativeProtocol and extract FakeNativeSystem

### Step 1 — Write failing test for protocol dispatch

**File:** `bindings/python/tests/unit/test_native_protocol.py` (new)

```python
import unittest
from hpactor._native_protocol import _NativeProtocol

class FakeBackend:
    """Minimal backend for protocol conformance testing."""
    def start(self) -> None: ...
    def stop(self) -> None: ...
    def begin_draining(self) -> None: ...
    def spawn_bridge(self) -> tuple: return ((0, b"", 0, 0, 1, 1), 1)
    def stop_bridge(self, addr: tuple) -> bool: return True
    def register_name(self, name: str, addr: tuple) -> bool: return True
    def resolve_name(self, name: str): return None
    def submit(self, cmd: dict) -> bool: return True
    def drain_dispatch(self, n: int) -> list: return []
    def drain_completions(self, n: int) -> list: return []
    def snapshot(self) -> dict: return {}
    @property
    def dispatch_fd(self) -> int: return -1
    @property
    def completion_fd(self) -> int: return -1
    def application_origin(self) -> tuple: return (0, b"", 0, 0, 0, 0)

class NativeProtocolTest(unittest.TestCase):
    def test_fake_backend_conforms_to_protocol(self):
        backend = FakeBackend()
        # All protocol methods must be callable without error
        backend.start()
        addr, gen = backend.spawn_bridge()
        self.assertEqual(len(addr), 6)
        self.assertGreater(gen, 0)
        backend.stop()
```

This test exercises the protocol shape without depending on `_hpactor`.

### Step 2 — Write RED test, verify it fails (protocol module doesn't exist)

```bash
cd bindings/python && python -m pytest tests/unit/test_native_protocol.py -v
```

Expected: `ModuleNotFoundError: No module named 'hpactor._native_protocol'`

### Step 3 — Implement _NativeProtocol

**File:** `bindings/python/hpactor/_native_protocol.py`

```python
"""Internal duck-typed protocol for native backends."""

from __future__ import annotations

from enum import IntEnum
from typing import Any, Dict, List, Optional, Tuple

# Address tuple: (family, packed_address, port, actor_type, actor_id, incarnation)
AddressTuple = Tuple[int, bytes, int, int, int, int]

# Dispatch dict keys: kind, actor, generation, type_tag, payload, sender,
#   message_id, ask_message_id, priority, deadline_ns, flags, ack_requested, sequence
DispatchDict = Dict[str, Any]

# Completion dict keys: kind, token, sequence, failure_reason, failure_source,
#   actor, generation, type_tag, payload, error_code, detail, schedule_handle,
#   delivery_status, retry_after_ns
CompletionDict = Dict[str, Any]

# Command dict keys: kind, token, sequence, generation, origin, target,
#   type_tag, payload, + optional fields
CommandDict = Dict[str, Any]


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


class _NativeBackend:
    """Duck-typed protocol that both FakeNativeSystem and Pybind11NativeSystem
    satisfy.  Not an ABC — structural subtyping for zero-overhead dispatch."""

    def start(self) -> None: ...
    def stop(self) -> None: ...
    def begin_draining(self) -> None: ...

    def spawn_bridge(self) -> Tuple[AddressTuple, int]: ...
    def stop_bridge(self, address: AddressTuple) -> bool: ...
    def register_name(self, name: str, address: AddressTuple) -> bool: ...
    def resolve_name(self, name: str) -> Optional[AddressTuple]: ...

    def submit(self, command: CommandDict) -> bool: ...
    def drain_dispatch(self, max_items: int) -> List[DispatchDict]: ...
    def drain_completions(self, max_items: int) -> List[CompletionDict]: ...

    def snapshot(self) -> Dict[str, Any]: ...

    @property
    def dispatch_fd(self) -> int: ...
    @property
    def completion_fd(self) -> int: ...

    def application_origin(self) -> AddressTuple: ...
```

### Step 4 — Extract FakeNativeSystem from _runtime.py

**File:** `bindings/python/hpactor/_native_fake.py`

Move the current fake implementation from `_system.py` and `_runtime.py` into a
single `FakeNativeSystem` class that satisfies `_NativeBackend`:

```python
class FakeNativeSystem:
    def __init__(self, registry, config, thread):
        self._registry = registry
        self._config = config
        self._thread = thread
        self._started = False

    def start(self) -> None:
        self._started = True

    def stop(self) -> None:
        self._started = False

    def begin_draining(self) -> None: ...

    def spawn_bridge(self) -> Tuple[AddressTuple, int]:
        actor_id = self._next_id()
        return ((0, b"", 0, 0, actor_id, 1), 1)

    def submit(self, command: CommandDict) -> bool:
        # Directly enqueue to the coordinator for local delivery
        ...
        return True

    def drain_dispatch(self, max_items: int) -> List[DispatchDict]:
        return []  # Fake mode: coordinator handles dispatch inline

    def drain_completions(self, max_items: int) -> List[CompletionDict]:
        return []  # Fake mode: completions resolved inline

    @property
    def dispatch_fd(self) -> int: return -1

    @property
    def completion_fd(self) -> int: return -1
    ...
```

This extraction is a pure refactor — the fake system behavior is byte-for-byte
identical to the current implementation.

### Step 5 — Verify GREEN

```bash
cd bindings/python && python -m pytest tests/unit/ -v
```

Expected: all 22 existing tests pass.

**Commit:** `refactor: extract _NativeProtocol and FakeNativeSystem`

---

## Task 2: Implement Pybind11NativeSystem

### Step 1 — Write failing test for real backend

**File:** `bindings/python/tests/unit/test_native_pybind11.py` (new)

```python
import unittest

class Pybind11NativeSystemTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from hpactor._native_pybind11 import Pybind11NativeSystem
        cls.Backend = Pybind11NativeSystem

    def test_construct_with_default_config(self):
        backend = self.Backend({})
        self.assertIsNotNone(backend)

    def test_start_stop_lifecycle(self):
        backend = self.Backend(self._default_config())
        backend.start()
        self.assertGreaterEqual(backend.dispatch_fd, 0)
        self.assertGreaterEqual(backend.completion_fd, 0)
        backend.stop()

    def test_spawn_bridge_returns_real_address(self):
        backend = self.Backend(self._default_config())
        backend.start()
        addr, gen = backend.spawn_bridge()
        self.assertEqual(len(addr), 6)
        self.assertEqual(addr[0], 4)  # IPv4 loopback
        self.assertGreater(addr[4], 0)  # actor_id > 0
        self.assertGreater(gen, 0)
        backend.stop()

    def test_drain_dispatch_returns_list(self):
        backend = self.Backend(self._default_config())
        backend.start()
        dispatches = backend.drain_dispatch(16)
        self.assertIsInstance(dispatches, list)
        backend.stop()

    def test_snapshot_has_expected_keys(self):
        backend = self.Backend(self._default_config())
        backend.start()
        snap = backend.snapshot()
        self.assertIn("state", snap)
        self.assertIn("dispatch_depth", snap)
        backend.stop()

    def _default_config(self):
        return {
            "dispatch_queue_capacity": 65536,
            "command_queue_capacity": 16384,
            "completion_queue_capacity": 16384,
            "max_actor_bindings": 256,
            "max_dispatch_per_tick": 256,
            "max_commands_per_turn": 256,
            "loop_lag_unready_ms": 5000,
            "handler_shutdown_timeout_ms": 10000,
            "trace_handler_spans": True,
        }
```

### Step 2 — Run tests to verify RED

```bash
cd bindings/python && python -m pytest tests/unit/test_native_pybind11.py -v
```

Expected: `ModuleNotFoundError: No module named 'hpactor._native_pybind11'`

### Step 3 — Implement Pybind11NativeSystem

**File:** `bindings/python/hpactor/_native_pybind11.py`

```python
"""Real native backend wrapping _hpactor.NativeSystem."""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

from ._native_protocol import (
    AddressTuple,
    CommandDict,
    CompletionDict,
    DispatchDict,
)


class Pybind11NativeSystem:
    """Wraps _hpactor.NativeSystem and implements the _NativeBackend protocol."""

    def __init__(self, config: Dict[str, Any]):
        self._config = config
        self._ns = None  # Lazy — created in start()

    def start(self) -> None:
        import _hpactor
        cfg = self._config or {}
        defaults = {
            "dispatch_queue_capacity": 65536,
            "command_queue_capacity": 16384,
            "completion_queue_capacity": 16384,
            "max_actor_bindings": 65536,
            "max_dispatch_per_tick": 256,
            "max_commands_per_turn": 256,
            "loop_lag_unready_ms": 5000,
            "handler_shutdown_timeout_ms": 10000,
            "trace_handler_spans": True,
        }
        for k, v in defaults.items():
            cfg.setdefault(k, v)
        self._ns = _hpactor.NativeSystem(cfg)
        self._ns.start()

    def stop(self) -> None:
        if self._ns:
            self._ns.stop()
            self._ns = None

    def begin_draining(self) -> None:
        if self._ns:
            self._ns.begin_draining()

    def spawn_bridge(self) -> Tuple[AddressTuple, int]:
        result = self._ns.spawn_bridge()
        addr_tup, generation = result
        return (
            (int(addr_tup[0]), bytes(addr_tup[1]), int(addr_tup[2]),
             int(addr_tup[3]), int(addr_tup[4]), int(addr_tup[5])),
            int(generation),
        )

    def stop_bridge(self, address: AddressTuple) -> bool:
        return self._ns.stop_bridge(address)

    def register_name(self, name: str, address: AddressTuple) -> bool:
        return self._ns.register_name(name, address)

    def resolve_name(self, name: str) -> Optional[AddressTuple]:
        result = self._ns.resolve_name(name)
        if result is None:
            return None
        return (
            int(result[0]), bytes(result[1]), int(result[2]),
            int(result[3]), int(result[4]), int(result[5]),
        )

    def submit(self, command: CommandDict) -> bool:
        return self._ns.submit(command)

    def drain_dispatch(self, max_items: int) -> List[DispatchDict]:
        return list(self._ns.drain_dispatch(max_items))

    def drain_completions(self, max_items: int) -> List[CompletionDict]:
        return list(self._ns.drain_completions(max_items))

    def snapshot(self) -> Dict[str, Any]:
        return dict(self._ns.snapshot())

    @property
    def dispatch_fd(self) -> int:
        return self._ns.dispatch_fd if self._ns else -1

    @property
    def completion_fd(self) -> int:
        return self._ns.completion_fd if self._ns else -1

    def application_origin(self) -> AddressTuple:
        result = self._ns.application_origin()
        return (
            int(result[0]), bytes(result[1]), int(result[2]),
            int(result[3]), int(result[4]), int(result[5]),
        )
```

### Step 4 — Run tests to verify GREEN

```bash
cd bindings/python && python -m pytest tests/unit/test_native_pybind11.py -v
```

Expected: 6/6 tests pass.

**Commit:** `feat: implement Pybind11NativeSystem wrapping _hpactor.NativeSystem`

---

## Task 3: Wire ActorSystem to _NativeProtocol

### Step 1 — Write RED integration test

**File:** `bindings/python/tests/integration/test_native_spawn_echo.py` (new)

```python
import asyncio
import unittest

from google.protobuf.wrappers_pb2 import StringValue

from hpactor import (
    Actor,
    ActorContext,
    ActorSystem,
    Behavior,
    MessageRegistry,
    actor,
)


@actor("echo")
class EchoActor(Actor):
    def behavior(self) -> Behavior:
        return Behavior().on_request(StringValue, StringValue, self._echo)

    async def _echo(self, msg: StringValue, ctx: ActorContext) -> StringValue:
        return StringValue(value=f"echo: {msg.value}")


class NativeSpawnEchoTest(unittest.IsolatedAsyncioTestCase):
    async def test_spawn_and_ask_round_trip(self):
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()

        async with ActorSystem(
            messages=registry, use_native=True
        ) as system:
            ref = await system.spawn(EchoActor, name="echo")
            self.assertIsNotNone(ref)
            self.assertGreater(ref.address.actor_id, 0)

            reply = await system.ask(
                ref, StringValue(value="hello"),
                response_type=StringValue, timeout=10.0,
            )
            self.assertEqual(reply.value, "echo: hello")
```

### Step 2 — Run test to verify RED

```bash
PYTHONPATH=bindings/python:$PYTHONPATH \
  python -m pytest bindings/python/tests/integration/test_native_spawn_echo.py -v
```

Expected: `use_native=True` path not implemented → `AttributeError` or `TypeError`.

### Step 3 — Update ActorSystem.__init__ for dual-mode

**File:** `bindings/python/hpactor/_system.py`

```python
class ActorSystem:
    def __init__(
        self,
        messages: MessageRegistry,
        *,
        config: Optional[Dict[str, Any]] = None,
        use_native: bool = False,  # NEW
    ):
        if not messages._frozen:
            raise ActorNotReadyError("MessageRegistry must be frozen before use")
        self._registry = messages
        self._config = config or {}
        self._use_native = use_native  # NEW
        self._thread: Optional[_RuntimeThread] = None
        self._native: Optional[Any] = None  # NEW: _NativeBackend
        self._closed = False
        self._runners: Dict[int, _ActorRunner] = {}
        self._refs: Dict[str, ActorRef] = {}
```

### Step 4 — Update ActorSystem.__aenter__ for dual-mode

```python
async def __aenter__(self) -> ActorSystem:
    if self._closed:
        raise SystemClosedError("ActorSystem has been closed")

    if self._use_native:
        # Real mode: create native backend, start it, wire to runtime thread
        from ._native_pybind11 import Pybind11NativeSystem
        self._native = Pybind11NativeSystem(self._config)
        self._native.start()
        dispatch_cap = self._config.get("dispatch_queue_capacity", 65536)
        self._thread = _RuntimeThread(
            self._registry,
            dispatch_capacity=dispatch_cap,
            native_backend=self._native,  # NEW param
        )
    else:
        # Fake mode: unchanged
        from ._native_fake import FakeNativeSystem
        dispatch_cap = self._config.get("dispatch_queue_capacity", 256)
        self._thread = _RuntimeThread(
            self._registry, dispatch_capacity=dispatch_cap
        )
        self._native = FakeNativeSystem(
            self._registry, self._config, self._thread
        )

    self._thread.start()
    return self
```

### Step 5 — Update ActorSystem.spawn() for real mode

```python
async def spawn(self, actor_class, *args, name="", **kwargs):
    if self._closed or not self._thread:
        raise SystemClosedError("ActorSystem not running")

    actor_name = name or getattr(actor_class, "__hpactor_actor_name__", "")
    if not actor_name:
        actor_name = actor_class.__name__

    # ── Real mode ──
    if self._use_native and self._native:
        return await self._spawn_native(
            actor_class, args, kwargs, actor_name)

    # ── Fake mode (unchanged) ──
    ...

async def _spawn_native(self, actor_class, args, kwargs, name):
    """Real-mode spawn: native bridge + Python actor."""
    from ._address import ActorAddress

    # Step 2: spawn_bridge on the runtime thread
    fut = self._thread.submit(self._native.spawn_bridge)
    addr_tuple, generation = await asyncio.wrap_future(fut)

    # Step 3: construct Python actor
    instance = actor_class(*args, **kwargs)
    instance._bind(self._registry)

    # Step 4-5: create runner and install
    addr = ActorAddress(
        family=addr_tuple[0], packed_address=addr_tuple[1],
        port=addr_tuple[2], actor_type=addr_tuple[3],
        actor_id=addr_tuple[4], incarnation=addr_tuple[5],
    )
    ref = ActorRef(address=addr, name=name, generation=generation)
    runner = _ActorRunner(instance, ref, self._registry)

    def _install():
        self._thread.coordinator.install(runner)
        self._runners[addr.actor_id] = runner
        self._refs[name] = ref

    fut = self._thread.submit(_install)
    await asyncio.wrap_future(fut)

    # Step 6: register name
    fut = self._thread.submit(
        self._native.register_name, name, addr_tuple)
    await asyncio.wrap_future(fut)

    # Step 7: on_start()
    fut = self._thread.submit(instance.on_start())
    await asyncio.wrap_future(fut)

    return ref
```

### Step 6 — Update ActorSystem.send() for real mode

```python
def send(self, target, message, *, options=None):
    if self._closed:
        raise SystemClosedError("ActorSystem closed")

    payload = self._registry.serialize(message)
    tag = self._registry.type_tag_for(type(message))

    if self._use_native and self._native:
        # Real mode: submit command through native bridge
        token = self._thread.runtime.allocate_token()
        origin = self._native.application_origin()
        command = {
            "kind": CommandKind.SEND,
            "token": token,
            "sequence": 0,  # application bridge sequences its own
            "generation": 0,
            "origin": origin,
            "target": (
                target.address.family, target.address.packed_address,
                target.address.port, target.address.actor_type,
                target.address.actor_id, target.address.incarnation,
            ),
            "type_tag": tag,
            "payload": payload,
        }
        self._native.submit(command)
        receipt = DeliveryReceipt()
        # Token resolves asynchronously via completion fd reader
        self._thread.runtime.register_delivery_token(token, receipt)
        return receipt

    # Fake mode: unchanged
    ...
```

### Step 7 — Update ActorSystem.__aexit__ for coordinated shutdown

```python
async def __aexit__(self, *args):
    if self._closed:
        return
    self._closed = True

    if self._use_native and self._native:
        # Coordinated shutdown: drain → stop runners → stop native → join
        self._native.begin_draining()
        # Cancel active handlers
        for runner in reversed(list(self._runners.values())):
            fut = self._thread.submit(runner.stop_once())
            if fut is not None and hasattr(fut, 'result'):
                try:
                    await asyncio.wrap_future(fut)
                except Exception:
                    pass
        self._runners.clear()
        self._refs.clear()
        if self._thread:
            self._thread.stop()
            self._thread = None
        self._native.stop()
    else:
        # Fake mode: unchanged
        ...

    self._native = None
```

### Step 8 — Run integration tests to verify GREEN

```bash
PYTHONPATH=bindings/python:$PYTHONPATH \
  python -m pytest bindings/python/tests/integration/test_native_spawn_echo.py -v
```

Expected: `test_spawn_and_ask_round_trip` passes.

### Step 9 — Verify existing tests unchanged

```bash
cd bindings/python && python -m pytest tests/unit/ -v
```

Expected: all 22 existing tests pass (fake mode is default).

**Commit:** `feat: wire ActorSystem to _NativeProtocol for dual-mode operation`

---

## Task 4: Wire _RuntimeThread for native fd readers

### Step 1 — Add native_mode support to _RuntimeThread

**File:** `bindings/python/hpactor/_runtime.py`

Add `native_backend` parameter to `_RuntimeThread.__init__`:

```python
class _RuntimeThread:
    def __init__(
        self,
        registry: MessageRegistry,
        dispatch_capacity: int = 256,
        native_backend=None,  # NEW: optional _NativeBackend
    ):
        self._registry = registry
        self._dispatch_capacity = dispatch_capacity
        self._native = native_backend  # NEW
        self._loop = None
        self._thread = None
        ...

    def start(self) -> None:
        def _run():
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
            self.coordinator = _DispatchCoordinator(self._dispatch_capacity)
            self.runtime = _ActorRuntime(self._registry, self.coordinator)
            self._started.set()

            if self._native:
                # Register native fd readers
                disp_fd = self._native.dispatch_fd
                comp_fd = self._native.completion_fd
                if disp_fd >= 0:
                    self._loop.add_reader(disp_fd, self._on_dispatch_readable)
                if comp_fd >= 0:
                    self._loop.add_reader(comp_fd, self._on_completion_readable)

            self._loop.run_forever()

        self._thread = threading.Thread(target=_run, daemon=True)
        self._thread.start()
        self._started.wait()

    def _on_dispatch_readable(self) -> None:
        max_batch = self._dispatch_capacity
        dispatches = self._native.drain_dispatch(max_batch)
        for d in dispatches:
            self.coordinator.enqueue(d)
        if dispatches:
            self.coordinator.schedule_next()

    def _on_completion_readable(self) -> None:
        max_batch = self._dispatch_capacity
        completions = self._native.drain_completions(max_batch)
        for c in completions:
            token = c["token"]
            if token in self.runtime._tokens:
                self.runtime._resolve_token(token, c)

    def stop(self) -> None:
        if self._loop:
            if self._native:
                disp_fd = self._native.dispatch_fd
                comp_fd = self._native.completion_fd
                if disp_fd >= 0:
                    self._loop.remove_reader(disp_fd)
                if comp_fd >= 0:
                    self._loop.remove_reader(comp_fd)
            self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread:
            self._thread.join(timeout=10.0)
```

### Step 2 — Run integration test

```bash
python -m pytest bindings/python/tests/integration/test_native_spawn_echo.py -v
```

**Commit:** `feat: wire _RuntimeThread for native fd readers`

---

## Task 5: Add integration tests for remaining scenarios

### New integration test files

- `test_native_ask_reply.py` — ask/response through real bridge
- `test_native_lifecycle.py` — `on_start`, `on_stop`, failed start rollback
- `test_native_shutdown.py` — clean shutdown with active actors
- `test_native_ordering.py` — per-actor FIFO ordering through real bridge

### Step — Write and verify each test

Each test follows the same pattern: create `ActorSystem(use_native=True)`,
spawn actors, exercise operations, assert clean shutdown.

```bash
python -m pytest bindings/python/tests/integration/ -v
```

**Commit:** `test: add native integration tests for spawn, ask, lifecycle, shutdown`

---

## Task 6: Full verification and update project memory

### Step 1 — Run all tests

```bash
# C++ tests
ctest --test-dir build -R 'PythonBinding' --output-on-failure -j4

# Python unit tests (fake mode, default)
cd bindings/python && python -m pytest tests/unit/ -v

# Python integration tests (real mode)
PYTHONPATH=bindings/python:$PYTHONPATH \
  python -m pytest bindings/python/tests/integration/ -v
```

Expected: all C++ tests pass, all Python unit tests pass, all integration tests pass.

### Step 2 — Update CLAUDE_MEMORY.md

Add entry:

```markdown
**Python Native Integration** ✅ Complete (2026-07-10)
- Dual-mode _NativeProtocol seam: FakeNativeSystem (default, unchanged)
  and Pybind11NativeSystem (wraps _hpactor.NativeSystem)
- ActorSystem(use_native=True) wires real bridge for spawn, send, ask
- _RuntimeThread fd readers pump native dispatch/completion notifiers
- Dict-based wire format for dispatch, completion, and command types
- 22 existing unit tests pass unchanged; new integration tests pass
- Design spec: docs/superpowers/specs/2026-07-10-python-native-integration-design.md
```

### Step 3 — Update design spec status

```markdown
**Status:** Implemented
```

**Commit:** `docs: record native integration completion`

---

## Plan Completion Checklist

- [ ] Task 1: `_NativeProtocol` defined, `FakeNativeSystem` extracted (refactor, no behavior change)
- [ ] Task 2: `Pybind11NativeSystem` implemented, 6/6 tests pass
- [ ] Task 3: `ActorSystem` wired to `_NativeProtocol` (spawn/send/ask/shutdown), integration test passes
- [ ] Task 4: `_RuntimeThread` native fd readers, dispatch/completion pumping
- [ ] Task 5: Integration tests for ask, lifecycle, shutdown, ordering
- [ ] Task 6: Full verification + project memory update
- [ ] All 22 existing Python unit tests pass (fake mode, default)
- [ ] New integration tests pass (real mode)
- [ ] All C++ PythonBinding tests pass (209/209)
- [ ] All architecture scans pass (196/196)
- [ ] `Python.h` / `pybind11` headers confined to `python_pybind11/`
- [ ] No changes to `_address.py`, `_errors.py`, `_delivery.py`, `_messages.py`, `_behavior.py`, `_actor.py`, `_context.py`
