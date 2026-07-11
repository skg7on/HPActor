# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Public ActorSystem async context manager and imperative API."""

from __future__ import annotations

import asyncio
from typing import Any, Dict, Optional, Type

from google.protobuf.message import Message

from ._actor import Actor
from ._address import ActorAddress, ActorRef
from ._context import ActorContext
from ._delivery import DeliveryOptions, DeliveryReceipt
from ._errors import ActorNotReadyError, SystemClosedError
from ._messages import MessageRegistry
from ._runtime import _ActorRunner, _RuntimeThread
from ._topology import (
    PythonTopologyPolicy,
    TopologyError,
    TopologyPhase,
    _TopologyFactoryManifest,
)


class _SystemMode:
    IMPERATIVE = "imperative"
    TOPOLOGY = "topology"


class ActorSystem:
    """Async context manager owning the Python actor runtime.

    Typical usage::

        messages = MessageRegistry()
        messages.register(MyProto, type_tag=0x1000)
        messages.freeze()

        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(MyActor, name="echo")
            receipt = system.send(ref, MyProto(value=7))
    """

    def __init__(
        self,
        messages: MessageRegistry,
        *,
        config: Optional[Dict[str, Any]] = None,
        use_native: bool = False,
    ):
        if not messages._frozen:
            raise ActorNotReadyError("MessageRegistry must be frozen before use")
        self._registry = messages
        self._config = config or {}
        self._use_native = use_native
        self._native: Any = None  # _NativeBackend
        self._thread: Optional[_RuntimeThread] = None
        self._closed = False
        self._runners: Dict[int, _ActorRunner] = {}
        self._refs: Dict[str, ActorRef] = {}
        # Topology state (populated by from_topology).
        self._mode: str = _SystemMode.IMPERATIVE
        self._topology_path: str = ""
        self._topology_policy: Optional[PythonTopologyPolicy] = None
        self._topology_manifest: Optional[_TopologyFactoryManifest] = None

    # ── Declarative topology constructor ──────────────────────────────────

    @classmethod
    def from_topology(
        cls,
        path: str,
        *,
        messages: MessageRegistry,
        policy: PythonTopologyPolicy,
        config: Optional[Dict[str, Any]] = None,
    ) -> ActorSystem:
        """Create an ActorSystem from a TOML topology file.

        Returns a side-effect-free instance — no parsing, imports, threads,
        or actors are created until ``__aenter__()``.
        """
        import os
        system = cls(messages=messages, config=config)
        system._mode = _SystemMode.TOPOLOGY
        system._topology_path = os.path.abspath(path)
        system._topology_policy = policy
        system._topology_manifest = _TopologyFactoryManifest()
        return system

    # ── Name resolution ──────────────────────────────────────────────────

    def resolve(self, name: str) -> ActorRef:
        """Resolve a committed topology actor name to an ActorRef.

        Only available after the system has entered Running state.
        Raises KeyError if the name is not registered.
        """
        if self._closed:
            raise SystemClosedError("ActorSystem closed")
        ref = self._refs.get(name)
        if ref is None:
            raise KeyError(name)
        return ref

    async def _enter_topology(self) -> ActorSystem:
        """Enter Running state via declarative topology bootstrap."""
        assert self._topology_manifest is not None
        assert self._topology_policy is not None

        try:
            # Step 1: Construct native system.
            from ._native_pybind11 import Pybind11NativeSystem
            self._native = Pybind11NativeSystem(self._config)
            self._native.start()

            self._thread = _RuntimeThread(
                self._registry,
                dispatch_capacity=self._config.get("dispatch_queue_capacity", 65536),
                native_backend=self._native,
            )
            self._thread.start()

            # Step 2: Prepare topology natively (releases GIL).
            descriptors = await asyncio.to_thread(
                self._native.prepare_topology, self._topology_path
            )

            if not descriptors:
                raise TopologyError(
                    TopologyPhase.NATIVE_PREPARE,
                    detail="no Python actors found in topology",
                )

            # Step 3: Preflight manifest on runtime loop.
            index_to_token = await self._topology_manifest.preflight(
                descriptors, self._topology_policy, self._registry
            )

            # Step 4: Build native bindings.
            bindings = [
                (idx, token, self._topology_manifest.record_for_token(token).args_fingerprint)
                for idx, token in index_to_token.items()
            ]
            policy_fp = self._topology_policy.fingerprint
            effective_fp = self._native.bind_topology_manifest(bindings, policy_fp)

            # Step 5: Start prepared topology (releases GIL).
            await asyncio.to_thread(self._native.start_prepared_topology)

            return self

        except Exception:
            # Best-effort cleanup.
            try:
                if self._thread:
                    self._thread.stop()
                    self._thread = None
            except Exception:
                pass
            try:
                if self._native:
                    self._native.stop()
                    self._native = None
            except Exception:
                pass
            raise

    # ── Async context manager ─────────────────────────────────────────────

    async def __aenter__(self) -> ActorSystem:
        if self._closed:
            raise SystemClosedError("ActorSystem has been closed")

        # ── Topology mode ─────────────────────────────────────────────────
        if self._mode == _SystemMode.TOPOLOGY:
            return await self._enter_topology()

        dispatch_cap = self._config.get("dispatch_queue_capacity", 256)

        if self._use_native:
            from ._native_pybind11 import Pybind11NativeSystem
            self._native = Pybind11NativeSystem(self._config)
            self._native.start()
            dispatch_cap = self._config.get("dispatch_queue_capacity", 65536)
            self._thread = _RuntimeThread(
                self._registry,
                dispatch_capacity=dispatch_cap,
                native_backend=self._native,
            )
        else:
            from ._native_fake import FakeNativeSystem
            self._native = FakeNativeSystem()
            self._thread = _RuntimeThread(
                self._registry, dispatch_capacity=dispatch_cap
            )

        self._thread.start()
        return self

    async def __aexit__(self, *args: Any) -> None:
        if self._closed:
            return
        self._closed = True

        if self._use_native and self._native is not None:
            self._native.begin_draining()

        # Stop runners in reverse spawn order.
        if self._thread:
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

        if self._use_native and self._native is not None:
            self._native.stop()
            self._native = None

    # ── Spawn ─────────────────────────────────────────────────────────────

    async def spawn(
        self,
        actor_class: Type[Actor],
        *args: Any,
        name: str = "",
        **kwargs: Any,
    ) -> ActorRef:
        if self._closed or not self._thread:
            raise SystemClosedError("ActorSystem not running")

        actor_name = name or getattr(actor_class, "__hpactor_actor_name__", "")
        if not actor_name:
            actor_name = actor_class.__name__

        if self._use_native and self._native is not None:
            return await self._spawn_native(
                actor_class, args, kwargs, actor_name)

        instance = actor_class(*args, **kwargs)
        instance._bind(self._registry)  # validates and freezes behavior

        # Create a synthetic address (real integration uses native spawn_bridge).
        addr = ActorAddress(
            family=0, packed_address=b"", port=0,
            actor_type=0, actor_id=id(instance) % (2**64),
            incarnation=1,
        )
        ref = ActorRef(address=addr, name=actor_name, generation=1)
        runner = _ActorRunner(instance, ref, self._registry)

        # Install on the runtime loop.
        def _install() -> None:
            self._thread.coordinator.install(runner)  # type: ignore[union-attr]
            self._runners[addr.actor_id] = runner
            self._refs[actor_name] = ref

        fut = self._thread.submit(_install)  # type: ignore[union-attr]
        if fut is not None:
            await asyncio.wrap_future(fut)

        # Run on_start.
        fut = self._thread.submit(instance.on_start())  # type: ignore[union-attr]
        if fut is not None:
            await asyncio.wrap_future(fut)

        return ref

    # ── Native spawn helper ───────────────────────────────────────────────

    async def _spawn_native(
        self,
        actor_class: Type[Actor],
        args: tuple,
        kwargs: dict,
        name: str,
    ) -> ActorRef:
        """Real-mode spawn: native bridge + Python actor."""
        assert self._native is not None
        assert self._thread is not None

        addr_tuple = None
        addr = None
        runner = None
        try:
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

            def _install() -> None:
                self._thread.coordinator.install(runner)  # type: ignore[union-attr]
                self._runners[addr.actor_id] = runner
                self._refs[name] = ref

            fut = self._thread.submit(_install)
            if fut is not None:
                await asyncio.wrap_future(fut)

            # Step 6: register name
            fut = self._thread.submit(
                self._native.register_name, name, addr_tuple)
            if fut is not None:
                await asyncio.wrap_future(fut)

            # Step 7: on_start()
            fut = self._thread.submit(instance.on_start())
            if fut is not None:
                await asyncio.wrap_future(fut)

            return ref
        except Exception:
            # Compensating cleanup on failure
            if addr_tuple is not None:
                try:
                    stop_fut = self._thread.submit(
                        self._native.stop_bridge, addr_tuple)
                    if stop_fut is not None and hasattr(stop_fut, 'result'):
                        await asyncio.wrap_future(stop_fut)
                except Exception:
                    pass
            if addr is not None:
                if addr.actor_id in self._runners:
                    del self._runners[addr.actor_id]
                if name in self._refs:
                    del self._refs[name]
            if runner is not None and not runner.stopped:
                # Stop the runner if it was installed; coordinator
                # will clean up its internal tracking on stop_once().
                try:
                    stop_fut = self._thread.submit(runner.stop_once())
                    if stop_fut is not None and hasattr(stop_fut, 'result'):
                        await asyncio.wrap_future(stop_fut)
                except Exception:
                    pass
            raise

    # ── Send / Ask ────────────────────────────────────────────────────────

    def send(
        self,
        target: ActorRef,
        message: Message,
        *,
        options: Optional[DeliveryOptions] = None,
    ) -> DeliveryReceipt:
        if self._closed:
            raise SystemClosedError("ActorSystem closed")
        receipt = DeliveryReceipt()
        payload = self._registry.serialize(message)
        # In real integration: enqueue dispatch to the target's bridge.
        # For unit tests: schedule delivery on the runtime loop.
        tag = self._registry.type_tag_for(type(message))
        sender_addr = ActorAddress(
            family=0, packed_address=b"", port=0,
            actor_type=0, actor_id=0, incarnation=0,
        )
        dispatch = (
            target, target.generation, type(message), payload,
            sender_addr, 0, 0, 1,
        )
        if self._thread:
            self._thread.coordinator.enqueue(dispatch)
            self._thread.coordinator.schedule_next()
        from ._delivery import DeliveryResult, DeliveryStatus
        receipt._resolve(DeliveryResult(status=DeliveryStatus.Accepted))
        return receipt

    async def ask(
        self,
        target: ActorRef,
        request: Message,
        response_type: Type[Message],
        *,
        timeout: float = 5.0,
    ) -> Message:
        if self._closed or not self._thread:
            raise SystemClosedError("ActorSystem not running")

        runtime = self._thread.runtime
        return await runtime.ask_command(
            target, request, response_type,
            int(timeout * 1000),
            origin=ActorRef(
                address=ActorAddress(
                    family=0, packed_address=b"", port=0,
                    actor_type=0, actor_id=0, incarnation=0,
                ),
                name="application",
            ),
        )

    # ── Introspection ─────────────────────────────────────────────────────

    @property
    def closed(self) -> bool:
        return self._closed
