# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Fake native backend for unit testing — no native module required."""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

from ._native_protocol import (
    AddressTuple,
    CommandDict,
    CompletionDict,
    CommandKind,
    DispatchDict,
)


class FakeNativeSystem:
    """In-process native backend for fast unit tests.

    Creates synthetic addresses, enqueues dispatches directly into the
    coordinator deque, and resolves completions inline.  No native
    threads, no bridge actors, no notifier fds.
    """

    def __init__(self):
        self._started = False
        self._next_id = 1

    # ── Lifecycle ────────────────────────────────────────────────────

    def start(self) -> None:
        self._started = True

    def stop(self) -> None:
        self._started = False

    def begin_draining(self) -> None:
        self._started = False

    # ── Actor management ─────────────────────────────────────────────

    def spawn_bridge(self) -> Tuple[AddressTuple, int]:
        actor_id = self._next_id
        self._next_id += 1
        return ((0, b"", 0, 0, actor_id, 1), 1)

    def stop_bridge(self, address: AddressTuple) -> bool:
        return True

    def register_name(self, name: str, address: AddressTuple) -> bool:
        return True

    def resolve_name(self, name: str) -> Optional[AddressTuple]:
        return None

    # ── Message passing ──────────────────────────────────────────────

    def submit(self, command: CommandDict) -> bool:
        return True

    def drain_dispatch(self, max_items: int) -> List[DispatchDict]:
        return []

    def drain_completions(self, max_items: int) -> List[CompletionDict]:
        return []

    # ── Observability ────────────────────────────────────────────────

    def snapshot(self) -> Dict[str, Any]:
        return {
            "state": 2 if self._started else 0,  # Running = 2
            "dispatch_depth": 0,
            "command_depth": 0,
            "completion_depth": 0,
            "dispatch_rejected": 0,
            "command_rejected": 0,
            "actor_bindings": 0,
        }

    @property
    def dispatch_fd(self) -> int:
        return -1

    @property
    def completion_fd(self) -> int:
        return -1

    def application_origin(self) -> AddressTuple:
        return (0, b"", 0, 0, 0, 0)
