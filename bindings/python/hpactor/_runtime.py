# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Bounded dispatch coordinator, actor runner, and dedicated event-loop thread."""

from __future__ import annotations

import asyncio
import collections
import concurrent.futures
import threading
from typing import Any, Callable, Dict, Optional, Tuple, Type

from google.protobuf.message import Message

from ._actor import Actor
from ._address import ActorAddress, ActorRef, ScheduleHandle
from ._behavior import Behavior
from ._context import ActorContext
from ._delivery import DeliveryReceipt, DeliveryResult, DeliveryStatus
from ._errors import (
    ActorError,
    ActorNotReadyError,
    AskCancelledError,
    AskTimeoutError,
    ResourceExhaustedError,
    SystemClosedError,
)
from ._messages import MessageRegistry
from ._topology import _TopologyFactoryManifest, TopologyActorOutcome


class _ActorRunner:
    """Serialized per-actor executor — one turn at a time, across await."""

    def __init__(self, actor: Actor, ref: ActorRef, registry: MessageRegistry):
        self._actor = actor
        self._ref = ref
        self._registry = registry
        self._last_sequence: int = 0
        self._stopped = False

    @property
    def ref(self) -> ActorRef:
        return self._ref

    @property
    def stopped(self) -> bool:
        return self._stopped

    async def run_one(
        self,
        dispatch: tuple,
        runtime: _ActorRuntime,
    ) -> None:
        """Execute exactly one handler for the given dispatch tuple."""
        if self._stopped:
            return
        # dispatch: (actor_ref, generation, msg_cls, payload, sender,
        #            msg_id, ask_id, sequence)
        (_ref, generation, msg_cls, payload, sender, msg_id,
         ask_id, sequence) = dispatch

        if generation != self._ref.generation:
            return  # stale generation — discard
        if sequence <= self._last_sequence:
            return  # duplicate/out-of-order
        self._last_sequence = sequence

        behavior = self._actor.current_behavior
        if not behavior or not behavior.frozen:
            return

        entry = behavior.handler_for(msg_cls)
        if entry is None:
            return  # no handler registered

        message = self._registry.deserialize(
            self._registry.type_tag_for(msg_cls), payload
        )
        ctx = ActorContext(
            runtime, self._ref, sender,
            ask_message_id=ask_id, turn_id=sequence,
        )
        try:
            result = await entry.handler(self._actor, message, ctx)
            if entry.response_class is not None and result is not None:
                runtime._resolve_ask(
                    ask_id if ask_id else msg_id,
                    entry.response_class,
                    result,
                )
        except Exception as exc:
            if entry.response_class is not None:
                runtime._fail_ask(
                    ask_id if ask_id else msg_id, exc
                )
        finally:
            ctx._expire()

    async def stop_once(self) -> None:
        if self._stopped:
            return
        self._stopped = True
        try:
            await self._actor.on_stop()
        except Exception:
            pass


class _DispatchCoordinator:
    """Bounded FIFO dispatch queue with per-actor non-reentrant scheduling."""

    def __init__(self, capacity: int):
        self._deque: collections.deque = collections.deque(maxlen=capacity)
        self._active: set[Tuple[int, int]] = set()  # (actor_id, generation)
        self._runners: Dict[Tuple[int, int], _ActorRunner] = {}
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    def set_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    def install(self, runner: _ActorRunner) -> None:
        key = (runner.ref.address.actor_id, runner.ref.generation)
        self._runners[key] = runner

    def remove(self, actor_id: int, generation: int) -> None:
        key = (actor_id, generation)
        self._runners.pop(key, None)
        self._active.discard(key)

    def enqueue(self, dispatch: Any) -> bool:
        """Append a dispatch tuple; returns False if full."""
        if len(self._deque) >= self._deque.maxlen:
            return False
        self._deque.append(dispatch)
        return True

    def schedule_next(self) -> None:
        """Scan the deque and schedule one turn per inactive actor."""
        if self._loop is None:
            return

        # Check for system dispatches (TopologyInstall=4, TopologyRollback=5).
        if self._deque:
            head = self._deque[0]
            if isinstance(head, dict):
                kind = head.get("kind", 0)
                if kind in (4, 5):  # TopologyInstall, TopologyRollback
                    self._deque.popleft()
                    self._handle_system_dispatch(head)
                    return

        scheduled = 0
        for _ in range(len(self._deque)):
            dispatch = self._deque[0]
            # dispatch is a complex tuple; extract actor key from first fields
            actor_id = dispatch[0].address.actor_id if hasattr(
                dispatch[0], 'address') else 0
            generation = dispatch[0].generation if hasattr(
                dispatch[0], 'generation') else 0
            key = (actor_id, generation)
            if key not in self._active:
                self._active.add(key)
                runner = self._runners.get(key)
                if runner and not runner.stopped:
                    self._loop.call_soon_threadsafe(
                        lambda r=runner, d=dispatch: asyncio.ensure_future(
                            r.run_one(d, self._runtime)
                        )
                    )
                self._deque.popleft()
                scheduled += 1
            else:
                self._deque.rotate(-1)  # move to end
            if scheduled > 0:
                break

    def _handle_system_dispatch(self, dispatch: dict) -> None:
        """Route a system dispatch to the appropriate runtime handler."""
        if self._loop is None or self._runtime is None:
            return
        kind = dispatch.get("kind", 0)
        if kind == 4:  # TopologyInstall
            self._loop.call_soon_threadsafe(
                lambda d=dispatch: asyncio.ensure_future(
                    self._runtime.install_topology_actor(d)))
        elif kind == 5:  # TopologyRollback
            self._loop.call_soon_threadsafe(
                lambda d=dispatch: asyncio.ensure_future(
                    self._runtime.rollback_topology_actor(d)))

    def on_turn_complete(self, actor_id: int, generation: int) -> None:
        key = (actor_id, generation)
        self._active.discard(key)
        self.schedule_next()


class _TokenRegistry:
    """Bounded token → future map for ask completions."""

    def __init__(self, capacity: int):
        self._capacity = capacity
        self._tokens: Dict[int, concurrent.futures.Future] = {}
        self._next_token = 1

    def allocate(self) -> int:
        if len(self._tokens) >= self._capacity:
            raise ResourceExhaustedError("token registry full")
        token = self._next_token
        self._next_token += 1
        self._tokens[token] = concurrent.futures.Future()
        return token

    def resolve(self, token: int, value: Any) -> None:
        fut = self._tokens.pop(token, None)
        if fut and not fut.done():
            fut.set_result(value)

    def fail(self, token: int, exc: Exception) -> None:
        fut = self._tokens.pop(token, None)
        if fut and not fut.done():
            fut.set_exception(exc)

    def cancel(self, token: int) -> None:
        fut = self._tokens.pop(token, None)
        if fut and not fut.done():
            fut.cancel()


class _ActorRuntime:
    """Internal protocol: bridges ActorContext methods to native commands."""

    def __init__(
        self,
        registry: MessageRegistry,
        coordinator: _DispatchCoordinator,
        tokens: _TokenRegistry,
        manifest: Optional[_TopologyFactoryManifest] = None,
        native_system: Any = None,
    ):
        self.registry = registry
        self.coordinator = coordinator
        self.tokens = tokens
        self._manifest = manifest
        self._native = native_system
        self._pending_deliveries: Dict[int, DeliveryReceipt] = {}
        # Track in-progress topology actors: {factory_token: (runner, task)}
        self._topology_tasks: Dict[int, Any] = {}

    # ── Command stubs (delegate to native system in real integration) ─────

    def send_command(self, kind: str, *, target: ActorRef, message: Message,
                     origin: ActorRef,
                     options: Any = None) -> DeliveryReceipt:
        receipt = DeliveryReceipt()
        # In real integration: serialize, allocate token, enqueue native command.
        # For unit tests: resolve immediately as Accepted.
        result = DeliveryResult(status=DeliveryStatus.Accepted)
        receipt._resolve(result)
        return receipt

    def reply_command(self, origin: ActorRef, sender: ActorAddress,
                      ask_id: int, message: Message) -> None:
        pass  # stub

    def reply_error_command(self, origin: ActorRef, sender: ActorAddress,
                            ask_id: int, code: int, detail: str) -> None:
        pass  # stub

    async def ask_command(
        self, target: ActorRef, request: Message,
        response_type: Type[Message], timeout_ms: int,
        origin: ActorRef,
    ) -> Message:
        token = self.tokens.allocate()
        # In real integration: enqueue native ASK command.
        try:
            fut = self.tokens._tokens.get(token)
            if fut is None:
                raise AskCancelledError("ask cancelled")
            result = await asyncio.wait_for(
                asyncio.wrap_future(fut),
                timeout=timeout_ms / 1000.0,
            )
            return result
        except asyncio.TimeoutError:
            self.tokens.fail(token, AskTimeoutError("ask timed out"))
            raise AskTimeoutError("ask timed out")

    async def spawn_command(
        self, actor_class: Type[Actor], *args: Any,
        name: str = "", **kwargs: Any,
    ) -> ActorRef:
        raise NotImplementedError("spawn from context")

    async def schedule_command(
        self, message: Message, *, delay_ms: int,
        target: ActorRef,
    ) -> ScheduleHandle:
        return ScheduleHandle(value=0)  # stub

    async def cancel_schedule_command(self, handle: ScheduleHandle) -> None:
        pass  # stub

    async def link_command(self, origin: ActorRef,
                           target: ActorRef) -> None:
        pass  # stub

    async def unlink_command(self, origin: ActorRef,
                             target: ActorRef) -> None:
        pass  # stub

    async def monitor_command(self, origin: ActorRef,
                              target: ActorRef) -> None:
        pass  # stub

    async def demonitor_command(self, origin: ActorRef,
                                target: ActorRef) -> None:
        pass  # stub

    async def stop_command(self, target: ActorRef) -> None:
        pass  # stub

    def passivate_command(self, origin: ActorRef) -> None:
        pass  # stub

    def become_command(self, origin: ActorRef,
                       behavior: Behavior) -> None:
        pass  # stub

    # ── Internal ──────────────────────────────────────────────────────────

    def _resolve_ask(self, ask_id: int, response_type: Type[Message],
                     result: Message) -> None:
        self.tokens.resolve(ask_id, result)

    def _fail_ask(self, ask_id: int, exc: Exception) -> None:
        self.tokens.fail(ask_id, exc)

    # ── Topology handlers (Phase 1E) ──────────────────────────────────────

    async def install_topology_actor(self, dispatch: dict) -> None:
        """Install a topology actor from a TopologyInstall dispatch."""
        factory_token = dispatch.get("factory_token", 0)
        if not factory_token or self._manifest is None or self._native is None:
            return

        record = self._manifest.record_for_token(factory_token)
        if record is None:
            return

        try:
            # Construct the actor.
            actor = record.actor_class(**dict(record.args))
        except Exception as exc:
            await self._complete_topology_outcome(
                factory_token, dispatch,
                TopologyActorOutcome.ConstructorFailed,
                str(exc)[:4096])
            return

        # Bind behavior.
        try:
            actor._bind(self.registry)
        except Exception as exc:
            await self._complete_topology_outcome(
                factory_token, dispatch,
                TopologyActorOutcome.BehaviorFailed,
                str(exc)[:4096])
            return

        # Build the ActorRef from the bridge address.
        from ._address import ActorRef
        actor_address = dispatch.get("actor")
        generation = dispatch.get("generation", 0)

        # actor from native is a tuple (family, packed_addr, port, ...)
        # Build an ActorAddress/ActorRef from the native tuple.
        if isinstance(actor_address, (list, tuple)):
            from ._address import ActorAddress
            family = actor_address[0]
            packed = actor_address[1]
            port = actor_address[2]
            addr = ActorAddress(
                family=family, packed_address=packed, port=port,
                actor_type=0, actor_id=getattr(actor, '_actor_id', 0),
                incarnation=generation)
            ref = ActorRef(address=addr, name="")
        else:
            ref = ActorRef(address=actor_address, name="")

        # Create runner and install.
        runner = _ActorRunner(actor, ref, self.registry)
        self.coordinator.install(runner)

        # Run on_start().
        try:
            await actor.on_start()
        except Exception as exc:
            await self._complete_topology_outcome(
                factory_token, dispatch,
                TopologyActorOutcome.StartFailed,
                str(exc)[:4096])
            return

        # Success — signal readiness.
        await self._complete_topology_outcome(
            factory_token, dispatch, TopologyActorOutcome.Ready, "")

    async def rollback_topology_actor(self, dispatch: dict) -> None:
        """Rollback a previously installed topology actor."""
        factory_token = dispatch.get("factory_token", 0)
        if not factory_token:
            return

        # Cancel any in-progress task.
        task_info = self._topology_tasks.pop(factory_token, None)
        if task_info is not None:
            runner = task_info.get("runner")
            if runner and not runner.stopped:
                try:
                    await runner.stop_once()
                except Exception:
                    pass

    async def _complete_topology_outcome(
        self, factory_token: int, dispatch: dict,
        outcome: Any, detail: str) -> None:
        """Call native.complete_topology_actor with the correct outcome."""
        if self._native is None:
            return
        try:
            oc_map = {
                TopologyActorOutcome.Ready: 0,
                TopologyActorOutcome.ConstructorFailed: 1,
                TopologyActorOutcome.BehaviorFailed: 2,
                TopologyActorOutcome.StartFailed: 3,
                TopologyActorOutcome.RolledBack: 4,
                TopologyActorOutcome.Cancelled: 5,
            }
            outcome_code = oc_map.get(outcome, 3)
            error_code = 0 if outcome == TopologyActorOutcome.Ready else 1
            self._native.complete_topology_actor({
                "factory_token": factory_token,
                "system_generation": dispatch.get("generation", 0),
                "actor_generation": dispatch.get("generation", 0),
                "outcome": outcome_code,
                "error_code": error_code,
                "detail": detail[:4096],
            })
        except Exception:
            pass


class _RuntimeThread:
    """Dedicated asyncio event-loop thread owning all Python actors."""

    def __init__(self, registry: MessageRegistry,
                 dispatch_capacity: int = 256,
                 native_backend=None,
                 manifest: Optional[_TopologyFactoryManifest] = None):
        self._registry = registry
        self._native = native_backend
        self._coordinator = _DispatchCoordinator(dispatch_capacity)
        self._tokens = _TokenRegistry(dispatch_capacity)
        self._runtime = _ActorRuntime(registry, self._coordinator,
                                       self._tokens, manifest=manifest,
                                       native_system=native_backend)
        self._coordinator._runtime = self._runtime
        self._thread: Optional[threading.Thread] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._ready = threading.Event()
        self._stop_requested = threading.Event()

    @property
    def runtime(self) -> _ActorRuntime:
        return self._runtime

    @property
    def coordinator(self) -> _DispatchCoordinator:
        return self._coordinator

    @property
    def loop(self) -> Optional[asyncio.AbstractEventLoop]:
        return self._loop

    def start(self) -> None:
        def _run() -> None:
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
            self._coordinator.set_loop(self._loop)
            # Register native fd readers if in real mode
            if self._native is not None:
                disp_fd = self._native.dispatch_fd
                comp_fd = self._native.completion_fd
                if disp_fd >= 0:
                    self._loop.add_reader(disp_fd, self._on_dispatch_readable)
                if comp_fd >= 0:
                    self._loop.add_reader(comp_fd, self._on_completion_readable)
            self._ready.set()
            while not self._stop_requested.is_set():
                self._loop.run_forever()
            # Remove native fd readers
            if self._native is not None:
                disp_fd = self._native.dispatch_fd
                comp_fd = self._native.completion_fd
                if disp_fd >= 0:
                    self._loop.remove_reader(disp_fd)
                if comp_fd >= 0:
                    self._loop.remove_reader(comp_fd)
            # Drain remaining tasks
            pending = asyncio.all_tasks(self._loop)
            for task in pending:
                task.cancel()
            self._loop.run_until_complete(
                asyncio.gather(*pending, return_exceptions=True)
            )
            self._loop.close()

        self._thread = threading.Thread(target=_run, daemon=True)
        self._thread.start()
        self._ready.wait()

    def stop(self) -> None:
        if self._loop is None:
            return
        self._stop_requested.set()
        self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=5.0)

    # ── Native fd reader callbacks ───────────────────────────────────

    def _on_dispatch_readable(self) -> None:
        """Called by asyncio when the native dispatch fd is readable."""
        if self._native is None:
            return
        try:
            max_batch = self._coordinator._capacity
            dispatches = self._native.drain_dispatch(max_batch)
            for d in dispatches:
                self._coordinator.enqueue(d)
            if dispatches:
                self._coordinator.schedule_next()
        except Exception:
            import traceback
            traceback.print_exc()
            # Remove reader on persistent failure to avoid spinning
            if (self._loop is not None and self._native is not None and
                    self._native.dispatch_fd >= 0):
                self._loop.remove_reader(self._native.dispatch_fd)

    def _on_completion_readable(self) -> None:
        """Called by asyncio when the native completion fd is readable."""
        if self._native is None:
            return
        try:
            max_batch = self._coordinator._capacity
            completions = self._native.drain_completions(max_batch)
            for c in completions:
                token = c.get("token", 0)
                if token and token in self._tokens._tokens:
                    self._tokens.resolve(token, c)
        except Exception:
            import traceback
            traceback.print_exc()
            if (self._loop is not None and self._native is not None and
                    self._native.completion_fd >= 0):
                self._loop.remove_reader(self._native.completion_fd)

    def submit(self, coro: Any) -> Any:
        """Submit a coroutine or callable to the runtime loop (thread-safe)."""
        if self._loop is None:
            raise SystemClosedError("runtime loop not running")
        if not asyncio.iscoroutine(coro):
            # Regular callable — run it directly via call_soon_threadsafe.
            return asyncio.run_coroutine_threadsafe(
                _run_callable(coro), self._loop
            )
        if threading.current_thread() == self._thread:
            return asyncio.ensure_future(coro)
        return asyncio.run_coroutine_threadsafe(coro, self._loop)

async def _run_callable(fn: Any) -> None:
    """Async wrapper that runs a synchronous callable."""
    fn()
