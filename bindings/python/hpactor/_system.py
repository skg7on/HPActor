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
    ):
        if not messages._frozen:
            raise ActorNotReadyError("MessageRegistry must be frozen before use")
        self._registry = messages
        self._config = config or {}
        self._thread: Optional[_RuntimeThread] = None
        self._closed = False
        self._runners: Dict[int, _ActorRunner] = {}
        self._refs: Dict[str, ActorRef] = {}

    # ── Async context manager ─────────────────────────────────────────────

    async def __aenter__(self) -> ActorSystem:
        if self._closed:
            raise SystemClosedError("ActorSystem has been closed")
        dispatch_cap = self._config.get("dispatch_queue_capacity", 256)
        self._thread = _RuntimeThread(
            self._registry, dispatch_capacity=dispatch_cap
        )
        self._thread.start()
        return self

    async def __aexit__(self, *args: Any) -> None:
        if self._closed:
            return
        self._closed = True
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
