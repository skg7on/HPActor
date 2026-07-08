# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Typed exceptions for the HPActor Python binding."""

from __future__ import annotations


class HPActorError(Exception):
    """Base exception for all HPActor Python binding errors."""

    def __init__(self, message: str = "", *, code: int = 0, detail: str = ""):
        super().__init__(message)
        self.code = code
        self.detail = detail


class ActorError(HPActorError):
    """An actor replied with an explicit error (reply_error)."""


class RegistrationError(HPActorError):
    """Protobuf message registration conflict or validation failure."""


class SerializationError(HPActorError):
    """Protobuf serialization or deserialization failed."""


class ActorNotReadyError(HPActorError):
    """Operation attempted on an actor not yet started or already stopped."""


class ResourceExhaustedError(HPActorError):
    """Queue capacity, token, or lease pool exhausted."""


class AskTimeoutError(HPActorError):
    """An ask() request timed out without a response."""


class AskCancelledError(HPActorError):
    """An ask() request was cancelled."""


class SystemClosedError(HPActorError):
    """Operation attempted after the ActorSystem was closed."""
