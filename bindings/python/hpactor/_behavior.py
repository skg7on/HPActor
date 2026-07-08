# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Immutable validated handler tables for Python actors."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Awaitable, Callable, Dict, Optional, Type

from google.protobuf.message import Message

from ._errors import ActorNotReadyError
from ._messages import MessageRegistry


@dataclass(frozen=True, slots=True)
class _HandlerEntry:
    """A single validated handler in a frozen behavior."""

    request_class: Type[Message]
    response_class: Optional[Type[Message]]
    handler: Callable[..., Awaitable[Any]]


class Behavior:
    """Immutable handler table built via fluent ``.on()``/``.on_request()``.

    Handlers are collected during construction; ``freeze(registry)`` validates
    all registered types and produces a read-only mapping keyed by protobuf
    type tag.  Mutation after freeze raises ``ActorNotReadyError``.
    """

    def __init__(self) -> None:
        self._frozen = False
        self._entries: Dict[int, _HandlerEntry] = {}
        self._pending: list[tuple[bool, Type[Message], Optional[Type[Message]],
                                  Any]] = []

    # ── Builder ──────────────────────────────────────────────────────────

    def on(self, message_type: Type[Message],
           handler: Callable[..., Awaitable[None]]) -> Behavior:
        """Register a fire-and-forget handler."""
        self._check_mutable()
        self._pending.append((False, message_type, None, handler))
        return self

    def on_request(
        self,
        request_type: Type[Message],
        response_type: Type[Message],
        handler: Callable[..., Awaitable[Message]],
    ) -> Behavior:
        """Register a request-response handler."""
        self._check_mutable()
        self._pending.append((True, request_type, response_type, handler))
        return self

    # ── Freeze ───────────────────────────────────────────────────────────

    def freeze(self, registry: MessageRegistry) -> None:
        """Validate and freeze all handlers against the registry."""
        self._check_mutable()
        seen_requests: set[int] = set()
        for is_req, req_cls, resp_cls, handler in self._pending:
            tag = registry.type_tag_for(req_cls)
            if is_req and resp_cls is not None:
                registry.type_tag_for(resp_cls)  # validate response type too
            if tag in seen_requests:
                raise ActorNotReadyError(
                    f"duplicate handler for tag 0x{tag:04X}"
                )
            seen_requests.add(tag)
            self._entries[tag] = _HandlerEntry(
                request_class=req_cls,
                response_class=resp_cls,
                handler=handler,
            )
        self._pending.clear()
        self._frozen = True

    # ── Query ────────────────────────────────────────────────────────────

    def handler_for(self, message_type: Type[Message]) -> Optional[_HandlerEntry]:
        """Return the handler entry for a message type, or None."""
        from ._messages import MessageRegistry as MR  # avoid circular import
        # We require the registry to have been used for freezing;
        # look up the tag from the descriptor.
        full_name = message_type.DESCRIPTOR.full_name
        for entry in self._entries.values():
            if entry.request_class.DESCRIPTOR.full_name == full_name:
                return entry
        return None

    @property
    def frozen(self) -> bool:
        return self._frozen

    def _check_mutable(self) -> None:
        if self._frozen:
            raise ActorNotReadyError("behavior is frozen")
