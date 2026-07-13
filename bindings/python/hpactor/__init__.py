# Copyright 2026 HPActor Contributors (Apache 2.0)
"""HPActor — C++20 event-based actor framework with Python bindings."""

import importlib as _importlib
import sys as _sys

from ._actor import Actor, actor
from ._version import __version__
from ._address import ActorAddress, ActorRef, ScheduleHandle
from ._behavior import Behavior
from ._context import ActorContext
from ._delivery import (
    DeliveryMode,
    DeliveryOptions,
    DeliveryReceipt,
    DeliveryResult,
    DeliveryStatus,
    FailureReason,
    FailureSource,
)
from ._errors import (
    ActorError,
    ActorNotReadyError,
    AskCancelledError,
    AskTimeoutError,
    HPActorError,
    NativeBindingUnavailable,
    RegistrationError,
    ResourceExhaustedError,
    SerializationError,
    SystemClosedError,
)
from ._messages import MessageRegistry
from ._topology import (
    PythonTopologyPolicy,
    TopologyError,
    TopologyPhase,
)

_NATIVE_EXPORTS = frozenset({"ActorSystem"})


def __getattr__(name: str) -> object:
    if name not in _NATIVE_EXPORTS:
        # Try submodule resolution first (e.g. _hpactor, _system, client).
        # importlib.import_module bypasses __getattr__, avoiding recursion.
        try:
            module = _importlib.import_module(f".{name}", __name__)
        except ImportError:
            raise AttributeError(
                f"module 'hpactor' has no attribute {name!r}"
            )
        globals()[name] = module
        return module
    # Lazy native export: may trigger _system → _runtime → _native_pybind11
    try:
        from ._system import ActorSystem as _ActorSystem
    except ImportError as exc:
        raise NativeBindingUnavailable(
            name=name,
            implementation=_sys.implementation.name,
            platform=_sys.platform,
        ) from exc
    value = _ActorSystem
    globals()[name] = value
    return value


__all__ = [
    "__version__",
    "Actor",
    "ActorAddress",
    "ActorContext",
    "ActorError",
    "ActorNotReadyError",
    "ActorRef",
    "ActorSystem",
    "AskCancelledError",
    "AskTimeoutError",
    "Behavior",
    "DeliveryMode",
    "DeliveryOptions",
    "DeliveryReceipt",
    "DeliveryResult",
    "DeliveryStatus",
    "FailureReason",
    "FailureSource",
    "HPActorError",
    "MessageRegistry",
    "NativeBindingUnavailable",
    "PythonTopologyPolicy",
    "RegistrationError",
    "ResourceExhaustedError",
    "ScheduleHandle",
    "SerializationError",
    "SystemClosedError",
    "TopologyError",
    "TopologyPhase",
    "actor",
]
