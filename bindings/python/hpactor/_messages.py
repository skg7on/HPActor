# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Protobuf message registry with freeze-before-use semantics."""

from __future__ import annotations

import hashlib
from typing import Any, Dict, Optional, Type

from google.protobuf.descriptor import Descriptor
from google.protobuf.message import Message

from ._errors import RegistrationError, SerializationError

# Valid application tag range: 0x1000 .. 0x00FFFFFF
_MIN_APP_TAG = 0x1000
_MAX_APP_TAG = 0x00FFFFFF


class _RegistryEntry:
    """Frozen descriptor for one registered message type."""

    __slots__ = ("tag", "full_name", "cls", "fingerprint")

    def __init__(
        self, tag: int, full_name: str, cls: Type[Message], fingerprint: bytes
    ) -> None:
        self.tag = tag
        self.full_name = full_name
        self.cls = cls
        self.fingerprint = fingerprint


class MessageRegistry:
    """Pre-start, freezeable map of TypeTag ↔ protobuf message class.

    Registration is idempotent for the same (tag, full_name, class, fingerprint).
    After ``freeze()``, all further registration is rejected. Serialization
    always uses ``deterministic=True``.
    """

    def __init__(self) -> None:
        self._frozen = False
        self._by_tag: Dict[int, _RegistryEntry] = {}
        self._by_name: Dict[str, _RegistryEntry] = {}

    # ── Registration ──────────────────────────────────────────────────────

    def register(
        self,
        message_class: Type[Message],
        *,
        type_tag: int,
    ) -> None:
        if self._frozen:
            raise RegistrationError("registry is frozen")

        if not (_MIN_APP_TAG <= type_tag <= _MAX_APP_TAG):
            raise RegistrationError(
                f"type_tag 0x{type_tag:04X} outside application range "
                f"[0x{_MIN_APP_TAG:04X}..0x{_MAX_APP_TAG:08X}]"
            )

        descriptor: Descriptor = message_class.DESCRIPTOR
        full_name: str = descriptor.full_name
        fingerprint = hashlib.sha256(
            full_name.encode("utf-8") + b"\0" + descriptor.file.serialized_pb
        ).digest()

        # Idempotency check: same (tag, name, class, fingerprint).
        existing_tag = self._by_tag.get(type_tag)
        if existing_tag is not None:
            if (
                existing_tag.full_name == full_name
                and existing_tag.cls is message_class
                and existing_tag.fingerprint == fingerprint
            ):
                return  # idempotent re-registration
            raise RegistrationError(
                f"type_tag 0x{type_tag:04X} already registered to "
                f"'{existing_tag.full_name}'"
            )

        existing_name = self._by_name.get(full_name)
        if existing_name is not None:
            raise RegistrationError(
                f"message type '{full_name}' already registered "
                f"under tag 0x{existing_name.tag:04X}"
            )

        entry = _RegistryEntry(type_tag, full_name, message_class, fingerprint)
        self._by_tag[type_tag] = entry
        self._by_name[full_name] = entry

    def freeze(self) -> None:
        """Lock the registry; no further registration is accepted."""
        self._frozen = True

    # ── Lookup ────────────────────────────────────────────────────────────

    def type_tag_for(self, message_class: Type[Message]) -> int:
        full_name = message_class.DESCRIPTOR.full_name
        entry = self._by_name.get(full_name)
        if entry is None:
            raise RegistrationError(
                f"message type '{full_name}' is not registered"
            )
        return entry.tag

    def class_for(self, type_tag: int) -> Optional[Type[Message]]:
        entry = self._by_tag.get(type_tag)
        return entry.cls if entry else None

    # ── Serialization ─────────────────────────────────────────────────────

    def serialize(self, message: Message) -> bytes:
        if not self._frozen:
            raise RegistrationError("registry must be frozen before use")
        return message.SerializeToString(deterministic=True)

    def deserialize(self, type_tag: int, payload: bytes) -> Message:
        if not self._frozen:
            raise RegistrationError("registry must be frozen before use")
        entry = self._by_tag.get(type_tag)
        if entry is None:
            raise SerializationError(
                f"no registered class for tag 0x{type_tag:04X}"
            )
        return entry.cls.FromString(payload)
