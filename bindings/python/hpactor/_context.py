# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Handler-scoped ActorContext for Python actors."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Optional, Type

from google.protobuf.message import Message

from ._address import ActorAddress, ActorRef, ScheduleHandle
from ._delivery import DeliveryOptions, DeliveryReceipt, DeliveryResult
from ._errors import (
    ActorError,
    ActorNotReadyError,
    AskCancelledError,
    AskTimeoutError,
)

if TYPE_CHECKING:
    from ._messages import MessageRegistry
    from ._runtime import _ActorRuntime


class ActorContext:
    """Exposes actor-owned operations during one handler turn.

    Created by the runtime before invoking a handler and expired immediately
    after.  All methods raise ``ActorNotReadyError`` after expiry.
    """

    def __init__(
        self,
        runtime: _ActorRuntime,
        self_ref: ActorRef,
        sender: ActorAddress,
        *,
        ask_message_id: int = 0,
        turn_id: int = 0,
    ) -> None:
        self._runtime = runtime
        self._self = self_ref
        self._sender = sender
        self._ask_id = ask_message_id
        self._turn_id = turn_id
        self._live = True

    # ── Guard ─────────────────────────────────────────────────────────────

    def _require_live_turn(self) -> None:
        if not self._live:
            raise ActorNotReadyError("context expired after handler turn")

    def _expire(self) -> None:
        self._live = False

    # ── Properties ────────────────────────────────────────────────────────

    @property
    def self(self) -> ActorRef:
        return self._self

    @property
    def sender(self) -> ActorAddress:
        return self._sender

    # ── Send / Reply ──────────────────────────────────────────────────────

    def send(
        self,
        target: ActorRef,
        message: Message,
        *,
        options: Optional[DeliveryOptions] = None,
    ) -> DeliveryReceipt:
        self._require_live_turn()
        return self._runtime.send_command(
            "send",
            target=target,
            message=message,
            origin=self._self,
            options=options,
        )

    def reply(self, message: Message) -> None:
        self._require_live_turn()
        self._runtime.reply_command(self._self, self._sender, self._ask_id,
                                     message)

    def reply_error(self, code: int, detail: str = "") -> None:
        self._require_live_turn()
        self._runtime.reply_error_command(
            self._self, self._sender, self._ask_id, code, detail
        )

    # ── Ask ───────────────────────────────────────────────────────────────

    async def ask(
        self,
        target: ActorRef,
        request: Message,
        response_type: Type[Message],
        *,
        timeout_ms: int = 5000,
    ) -> Message:
        self._require_live_turn()
        return await self._runtime.ask_command(
            target, request, response_type, timeout_ms, origin=self._self
        )

    # ── Spawn / Schedule / Lifecycle ──────────────────────────────────────

    async def spawn(
        self, actor_class: Type[Any], *args: Any, name: str = "", **kwargs: Any
    ) -> ActorRef:
        self._require_live_turn()
        return await self._runtime.spawn_command(
            actor_class, *args, name=name, **kwargs
        )

    async def schedule(
        self,
        message: Message,
        *,
        delay_ms: int,
        target: Optional[ActorRef] = None,
    ) -> ScheduleHandle:
        self._require_live_turn()
        return await self._runtime.schedule_command(
            message, delay_ms=delay_ms, target=target or self._self
        )

    async def cancel_schedule(self, handle: ScheduleHandle) -> None:
        self._require_live_turn()
        await self._runtime.cancel_schedule_command(handle)

    async def link(self, target: ActorRef) -> None:
        self._require_live_turn()
        await self._runtime.link_command(self._self, target)

    async def unlink(self, target: ActorRef) -> None:
        self._require_live_turn()
        await self._runtime.unlink_command(self._self, target)

    async def monitor(self, target: ActorRef) -> None:
        self._require_live_turn()
        await self._runtime.monitor_command(self._self, target)

    async def demonitor(self, target: ActorRef) -> None:
        self._require_live_turn()
        await self._runtime.demonitor_command(self._self, target)

    async def stop(self, target: ActorRef) -> None:
        self._require_live_turn()
        await self._runtime.stop_command(target)

    def passivate(self) -> None:
        self._require_live_turn()
        self._runtime.passivate_command(self._self)

    def become(self, behavior: Any) -> None:
        self._require_live_turn()
        self._runtime.become_command(self._self, behavior)
