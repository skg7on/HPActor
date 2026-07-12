# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Test actors for declarative topology tests."""

import asyncio
from hpactor import Actor, actor


@actor("echo")
class EchoActor(Actor):
    """Actor that echoes string values with an optional prefix."""

    def __init__(self, prefix: str = ""):
        super().__init__()
        self._prefix = prefix
        self._started = False

    async def on_start(self) -> None:
        self._started = True

    def behavior(self):
        from hpactor import Behavior
        b = Behavior()
        # EchoString handler
        b.on("StringValue", self._echo)
        return b

    async def _echo(self, msg, ctx):
        from hpactor._messages import Message
        result = type(msg)()
        # Concatenate prefix + value if msg has 'value' attribute.
        if hasattr(msg, 'value') and hasattr(result, 'value'):
            result.value = self._prefix + msg.value
        await ctx.reply(result)


@actor("constructor_fails")
class ConstructorFails(Actor):
    """Actor that raises in __init__ for testing rollback."""

    def __init__(self):
        raise RuntimeError("constructor failure")


@actor("start_fails")
class StartFails(Actor):
    """Actor whose on_start raises for testing rollback."""

    async def on_start(self) -> None:
        raise RuntimeError("start failure")

    def behavior(self):
        from hpactor import Behavior
        return Behavior()


@actor("start_blocks")
class StartBlocks(Actor):
    """Actor whose on_start blocks until an event is set (for timeout testing)."""

    def __init__(self):
        super().__init__()
        self._block_event = asyncio.Event()

    async def on_start(self) -> None:
        await self._block_event.wait()

    def behavior(self):
        from hpactor import Behavior
        b = Behavior()
        b.on("StringValue", self._nop)
        return b

    async def _nop(self, msg, ctx):
        pass
