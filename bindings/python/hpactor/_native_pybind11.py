# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Real native backend wrapping _hpactor.NativeSystem (pybind11-based)."""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

from ._native_protocol import (
    AddressTuple,
    CommandDict,
    CompletionDict,
    DispatchDict,
)


class Pybind11NativeSystem:
    """Wraps _hpactor.NativeSystem and implements the _NativeBackend protocol.

    The native module is imported lazily in start(), so importing this
    module does not require _hpactor to be available.
    """

    def __init__(self, config: Optional[Dict[str, Any]] = None):
        self._config = config or {}
        self._ns = None  # _hpactor.NativeSystem — created in start()

    # ── Lifecycle ────────────────────────────────────────────────────

    def start(self) -> None:
        from . import _hpactor  # relative import — .so is inside the package

        cfg: Dict[str, Any] = {
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
        cfg.update(self._config)
        self._ns = _hpactor.NativeSystem(cfg)
        self._ns.start()

    def stop(self) -> None:
        if self._ns is not None:
            self._ns.stop()
            self._ns = None

    def begin_draining(self) -> None:
        if self._ns is not None:
            self._ns.begin_draining()

    # ── Actor management ─────────────────────────────────────────────

    def spawn_bridge(self) -> Tuple[AddressTuple, int]:
        if self._ns is None:
            raise RuntimeError(
                "Pybind11NativeSystem.spawn_bridge() called before start()")
        result = self._ns.spawn_bridge()
        addr_tup, generation = result
        return (
            (int(addr_tup[0]), bytes(addr_tup[1]), int(addr_tup[2]),
             int(addr_tup[3]), int(addr_tup[4]), int(addr_tup[5])),
            int(generation),
        )

    def stop_bridge(self, address: AddressTuple) -> bool:
        if self._ns is None:
            return False
        return self._ns.stop_bridge(address)

    def register_name(self, name: str, address: AddressTuple) -> bool:
        if self._ns is None:
            return False
        return self._ns.register_name(name, address)

    def resolve_name(self, name: str) -> Optional[AddressTuple]:
        if self._ns is None:
            return None
        result = self._ns.resolve_name(name)
        if result is None:
            return None
        return (
            int(result[0]), bytes(result[1]), int(result[2]),
            int(result[3]), int(result[4]), int(result[5]),
        )

    # ── Message passing ──────────────────────────────────────────────

    def submit(self, command: CommandDict) -> bool:
        if self._ns is None:
            return False
        return self._ns.submit(command)

    def drain_dispatch(self, max_items: int) -> List[DispatchDict]:
        if self._ns is None:
            return []
        return list(self._ns.drain_dispatch(max_items))

    def drain_completions(self, max_items: int) -> List[CompletionDict]:
        if self._ns is None:
            return []
        return list(self._ns.drain_completions(max_items))

    # ── Observability ────────────────────────────────────────────────

    def snapshot(self) -> Dict[str, Any]:
        if self._ns is None:
            return {}
        return dict(self._ns.snapshot())

    @property
    def dispatch_fd(self) -> int:
        return self._ns.dispatch_fd if self._ns is not None else -1

    @property
    def completion_fd(self) -> int:
        return self._ns.completion_fd if self._ns is not None else -1

    def application_origin(self) -> AddressTuple:
        if self._ns is None:
            return (0, b"", 0, 0, 0, 0)
        result = self._ns.application_origin()
        return (
            int(result[0]), bytes(result[1]), int(result[2]),
            int(result[3]), int(result[4]), int(result[5]),
        )
