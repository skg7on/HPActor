# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Actor base class and @actor decorator."""

from __future__ import annotations

import abc
from typing import Any, ClassVar, Optional

from ._behavior import Behavior
from ._context import ActorContext
from ._errors import ActorNotReadyError


def actor(name: str):
    """Decorator that sets the actor's runtime name.

    ``name`` must be non-empty ASCII, ≤ 255 bytes.
    """

    if not name or not name.isascii():
        raise ValueError("actor name must be non-empty ASCII")
    if len(name.encode("utf-8")) > 255:
        raise ValueError("actor name must be ≤ 255 bytes")

    def decorator(cls):
        cls.__hpactor_actor_name__ = name
        return cls

    return decorator


class Actor(abc.ABC):
    """Base class for all Python actors.

    Subclasses must implement ``behavior()`` and are spawned via
    ``ActorSystem.spawn()``.  Lifecycle hooks ``on_start()`` and
    ``on_stop()`` are optional async no-ops by default.
    """

    # Set by @actor(name) decorator.
    __hpactor_actor_name__: ClassVar[str] = ""

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)  # type: ignore[call-arg]
        self._current_behavior: Optional[Behavior] = None
        self._bound: bool = False

    # ── Lifecycle hooks ──────────────────────────────────────────────────

    async def on_start(self) -> None:
        """Called once before the first message is delivered."""

    async def on_stop(self) -> None:
        """Called once when the actor is stopping."""

    # ── Behavior ─────────────────────────────────────────────────────────

    @abc.abstractmethod
    def behavior(self) -> Behavior:
        """Return the initial behavior for this actor."""

    @property
    def current_behavior(self) -> Optional[Behavior]:
        return self._current_behavior

    def become(self, behavior: Behavior) -> None:
        """Replace the current behavior at runtime."""
        if not self._bound:
            raise ActorNotReadyError(
                "cannot become() before actor is bound to a runtime"
            )
        if not behavior.frozen:
            raise ActorNotReadyError("new behavior must be frozen")
        self._current_behavior = behavior

    # ── Internal ─────────────────────────────────────────────────────────

    def _bind(self, registry: Any = None) -> None:
        """Bind this actor to a runtime (called once before on_start)."""
        if self._bound:
            raise ActorNotReadyError("actor is already bound")
        self._bound = True
        self._current_behavior = self.behavior()
        if registry is not None:
            self._current_behavior.freeze(registry)
