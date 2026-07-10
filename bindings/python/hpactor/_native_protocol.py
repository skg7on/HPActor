# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Internal duck-typed protocol for native backends.

Provides CommandKind enum, type aliases for address/dispatch/completion
dicts, and the _NativeBackend structural interface that both
FakeNativeSystem and Pybind11NativeSystem satisfy.
"""

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
    """Command kinds matching PythonCommandKind in the native bridge."""

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
    satisfy.

    Not an ABC — structural subtyping for zero-overhead dispatch.  Each
    concrete backend implements the full set of methods.
    """

    def start(self) -> None: ...  # pragma: no cover
    def stop(self) -> None: ...  # pragma: no cover
    def begin_draining(self) -> None: ...  # pragma: no cover

    def spawn_bridge(self) -> Tuple[AddressTuple, int]: ...  # pragma: no cover
    def stop_bridge(self, address: AddressTuple) -> bool: ...  # pragma: no cover
    def register_name(self, name: str, address: AddressTuple) -> bool: ...  # pragma: no cover
    def resolve_name(self, name: str) -> Optional[AddressTuple]: ...  # pragma: no cover

    def submit(self, command: CommandDict) -> bool: ...  # pragma: no cover
    def drain_dispatch(self, max_items: int) -> List[DispatchDict]: ...  # pragma: no cover
    def drain_completions(self, max_items: int) -> List[CompletionDict]: ...  # pragma: no cover

    def snapshot(self) -> Dict[str, Any]: ...  # pragma: no cover

    @property
    def dispatch_fd(self) -> int: ...  # pragma: no cover
    @property
    def completion_fd(self) -> int: ...  # pragma: no cover

    def application_origin(self) -> AddressTuple: ...  # pragma: no cover
