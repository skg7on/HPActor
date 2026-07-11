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


# ── Backend protocol (duck-typed, not an ABC) ───────────────────────────
#
# Both FakeNativeSystem and Pybind11NativeSystem satisfy this structural
# interface:
#
#   start() -> None
#   stop() -> None
#   begin_draining() -> None
#   spawn_bridge() -> Tuple[AddressTuple, int]
#   stop_bridge(address: AddressTuple) -> bool
#   register_name(name: str, address: AddressTuple) -> bool
#   resolve_name(name: str) -> Optional[AddressTuple]
#   submit(command: CommandDict) -> bool
#   drain_dispatch(max_items: int) -> List[DispatchDict]
#   drain_completions(max_items: int) -> List[CompletionDict]
#   snapshot() -> Dict[str, Any]
#   dispatch_fd: int (property)
#   completion_fd: int (property)
#   application_origin() -> AddressTuple
