#!/usr/bin/env python3
"""Supervision example — restart on failure with generation fencing.

Demonstrates an actor that fails intentionally, gets restarted by the
supervisor, and how generation fencing prevents stale work.
"""

import asyncio

from google.protobuf.wrappers_pb2 import StringValue

from hpactor import Actor, ActorSystem, Behavior, MessageRegistry


class Worker(Actor):
    def __init__(self, fail_on_first: bool = True):
        super().__init__()
        self._fail_on_first = fail_on_first

    def on_restart(self) -> None:
        self._fail_on_first = False

    def behavior(self) -> Behavior:
        return Behavior().on(StringValue, self.handle)

    async def handle(self, msg: StringValue, ctx) -> None:
        if self._fail_on_first:
            raise RuntimeError("Intentional failure — will restart")
        print(f"Worker received after restart: {msg.value}")


async def main() -> None:
    messages = MessageRegistry()
    messages.register(StringValue, type_tag=0x1000)

    async with ActorSystem(messages=messages) as system:
        ref = await system.spawn(Worker, name="worker")

        # First message triggers failure + restart
        await system.send(ref, StringValue(value="first"))
        await asyncio.sleep(0.2)

        # Second message reaches the restarted actor
        await system.send(ref, StringValue(value="second"))
        await asyncio.sleep(0.2)

    print("System shut down cleanly")


if __name__ == "__main__":
    asyncio.run(main())
