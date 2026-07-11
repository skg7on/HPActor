# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Declarative topology: policy, preflight, and factory manifest."""

from __future__ import annotations

import importlib
import inspect
from dataclasses import dataclass, field
from enum import Enum
from types import MappingProxyType
from typing import TYPE_CHECKING, Dict, List, Optional, Sequence, Tuple, Type

from ._errors import HPActorError

if TYPE_CHECKING:
    from ._actor import Actor
    from ._messages import MessageRegistry

# ── FNV-1a 64-bit fingerprint (matches native implementation) ───────────────

_FNV_OFFSET: int = 0xCBF29CE484222325
_FNV_PRIME: int = 0x100000001B3


def _fnv1a64_length_prefixed(parts: Sequence[bytes]) -> int:
    """FNV-1a hash over length-prefixed byte sequences (deterministic)."""
    h = _FNV_OFFSET
    for part in parts:
        length = len(part).to_bytes(8, "little")
        for byte in length + part:
            h = ((h ^ byte) * _FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


# ── Module name validation ──────────────────────────────────────────────────

def _is_absolute_module_name(name: str) -> bool:
    """Check that a name matches the absolute Python module grammar."""
    if not name:
        return False
    for segment in name.split("."):
        if not segment or not segment[0].isascii():
            return False
        if not (segment[0].isalpha() or segment[0] == "_"):
            return False
        for ch in segment[1:]:
            if not (ch.isalnum() or ch == "_"):
                return False
    return True


# ── Public API ──────────────────────────────────────────────────────────────


class TopologyPhase(Enum):
    """Phase in the topology lifecycle (mirrors native PythonTopologyPhase)."""
    PARSE = "parse"
    POLICY = "policy"
    IMPORT = "import"
    CLASS_RESOLUTION = "class_resolution"
    CLASS_VALIDATION = "class_validation"
    CONSTRUCTOR_BINDING = "constructor_binding"
    NATIVE_PREPARE = "native_prepare"
    ACTOR_START = "actor_start"
    COMMIT = "commit"
    ROLLBACK = "rollback"


class TopologyError(HPActorError):
    """Error during declarative topology loading."""

    def __init__(
        self,
        phase: TopologyPhase,
        *,
        actor_id: Optional[str] = None,
        behavior: Optional[str] = None,
        error_code: int = 0,
        detail: str = "",
        rollback_bits: int = 0,
    ) -> None:
        super().__init__(detail or f"topology error in phase {phase.value}")
        self.phase = phase
        self.actor_id = actor_id
        self.behavior = behavior
        self.error_code = error_code
        self.detail = detail[:4096]
        self.rollback_bits = rollback_bits


@dataclass(frozen=True, slots=True)
class PythonTopologyPolicy:
    """Application-side allowlist for Python topology imports."""

    allowed_module_prefixes: Tuple[str, ...]

    def __post_init__(self) -> None:
        prefixes = tuple(dict.fromkeys(self.allowed_module_prefixes))
        if not prefixes:
            raise ValueError(
                "allowed_module_prefixes must be non-empty when Python "
                "actors are declared"
            )
        for p in prefixes:
            if not _is_absolute_module_name(p):
                raise ValueError(
                    f"invalid module prefix: {p!r}"
                )
        object.__setattr__(self, "allowed_module_prefixes", prefixes)

    def allows(self, module: str) -> bool:
        """Return True if `module` is allowed by this policy."""
        return any(
            module == prefix or module.startswith(prefix + ".")
            for prefix in self.allowed_module_prefixes
        )

    @property
    def fingerprint(self) -> int:
        """Deterministic FNV-1a fingerprint of the sorted allowlist."""
        parts = tuple(
            sorted(p.encode("utf-8") for p in self.allowed_module_prefixes)
        )
        return _fnv1a64_length_prefixed(parts)


# ── Internal factory manifest ───────────────────────────────────────────────


@dataclass(frozen=True, slots=True)
class _TopologyFactoryRecord:
    """Immutable record mapping a factory token to an actor class and args."""

    topology_index: int
    factory_token: int
    actor_class: Type[Actor]
    args: MappingProxyType[str, str]
    args_fingerprint: int


class _TopologyFactoryManifest:
    """Imports, validates, and freezes factory records for Python actors."""

    def __init__(self) -> None:
        self._records: Dict[int, _TopologyFactoryRecord] = {}
        self._frozen: bool = False
        self._next_token: int = 1

    @property
    def frozen(self) -> bool:
        return self._frozen

    def token_for(self, topology_index: int) -> int:
        for rec in self._records.values():
            if rec.topology_index == topology_index:
                return rec.factory_token
        raise KeyError(f"no factory record for topology index {topology_index}")

    def record_for_token(self, factory_token: int) -> _TopologyFactoryRecord:
        return self._records[factory_token]

    async def preflight(
        self,
        descriptors: List[Tuple],
        policy: PythonTopologyPolicy,
        registry: MessageRegistry,
    ) -> Dict[int, int]:
        """Import modules, validate classes, freeze records.

        Args:
            descriptors: List of tuples from native prepare_topology().
            policy: Application allowlist.
            registry: Frozen message registry.

        Returns:
            Dict mapping topology_index → factory_token.
        """
        import asyncio

        # Verify we're on the dedicated runtime loop.
        loop = asyncio.get_running_loop()

        index_to_token: Dict[int, int] = {}
        imported_modules: Dict[str, object] = {}

        for desc in descriptors:
            (
                topology_index, actor_id, behavior,
                module, qualname, args_tuple, args_fingerprint,
            ) = desc

            # Step 1: Check policy.
            if not policy.allows(module):
                raise TopologyError(
                    TopologyPhase.POLICY,
                    actor_id=actor_id,
                    behavior=behavior,
                )

            # Step 2: Import module (once per unique module).
            if module not in imported_modules:
                try:
                    imported_modules[module] = importlib.import_module(module)
                except Exception as exc:
                    raise TopologyError(
                        TopologyPhase.IMPORT,
                        actor_id=actor_id,
                        behavior=behavior,
                        detail=f"import {module}: {exc}",
                    ) from exc

            mod = imported_modules[module]

            # Step 3: Resolve qualified class name.
            try:
                obj = mod
                for segment in qualname.split("."):
                    obj = getattr(obj, segment)
            except AttributeError as exc:
                raise TopologyError(
                    TopologyPhase.CLASS_RESOLUTION,
                    actor_id=actor_id,
                    behavior=behavior,
                    detail=f"resolve {module}:{qualname}: {exc}",
                ) from exc

            # Step 4: Validate class.
            if not inspect.isclass(obj):
                raise TopologyError(
                    TopologyPhase.CLASS_VALIDATION,
                    actor_id=actor_id,
                    behavior=behavior,
                    detail=f"{module}:{qualname} is not a class",
                )

            from ._actor import Actor as ActorBase
            if not issubclass(obj, ActorBase):
                raise TopologyError(
                    TopologyPhase.CLASS_VALIDATION,
                    actor_id=actor_id,
                    behavior=behavior,
                    detail=f"{module}:{qualname} is not an Actor subclass",
                )
            if inspect.isabstract(obj):
                raise TopologyError(
                    TopologyPhase.CLASS_VALIDATION,
                    actor_id=actor_id,
                    behavior=behavior,
                    detail=f"{module}:{qualname} is abstract",
                )
            if not getattr(obj, "__hpactor_actor_name__", None):
                raise TopologyError(
                    TopologyPhase.CLASS_VALIDATION,
                    actor_id=actor_id,
                    behavior=behavior,
                    detail=f"{module}:{qualname} missing @actor decorator",
                )

            # Step 5: Validate constructor kwargs.
            args_dict = dict(args_tuple)
            try:
                sig = inspect.signature(obj)
                sig.bind(**args_dict)
            except TypeError as exc:
                raise TopologyError(
                    TopologyPhase.CONSTRUCTOR_BINDING,
                    actor_id=actor_id,
                    behavior=behavior,
                    detail=f"constructor binding: {exc}",
                ) from exc

            # Step 6: Freeze factory record.
            token = self._next_token
            self._next_token += 1
            self._records[token] = _TopologyFactoryRecord(
                topology_index=topology_index,
                factory_token=token,
                actor_class=obj,
                args=MappingProxyType(dict(args_dict)),
                args_fingerprint=args_fingerprint,
            )
            index_to_token[topology_index] = token

        self._frozen = True
        return index_to_token
