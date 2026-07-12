# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Redacted bounded request-event delivery and hook isolation."""

from __future__ import annotations

import logging
from typing import Callable, Optional

from .models import RequestEvent, ResultCategory

_logger = logging.getLogger("hpactor.client")


def _deliver_event(
    hook: Optional[Callable[[RequestEvent], None]],
    event: RequestEvent,
) -> None:
    """Deliver *event* to *hook*, isolating hook failures."""
    if hook is None:
        return
    try:
        hook(event)
    except BaseException:
        if isinstance(__import__("sys").exc_info()[0], (KeyboardInterrupt, SystemExit)):
            raise
        _logger.exception("client event hook raised an exception")
