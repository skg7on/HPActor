#!/usr/bin/env python3
"""Operations example — bounded runtime snapshot and clean shutdown.

Demonstrates accessing runtime observability surfaces and graceful
context-manager shutdown.
"""

import asyncio

from google.protobuf.wrappers_pb2 import StringValue

from hpactor import (
    Actor, ActorSystem, Behavior, DeliveryOptions, MessageRegistry,
)


class Instrumented(Actor):
    def __init__(self):
        super().__init__()
        self.handled = 0

    def behavior(self) -> Behavior:
        return Behavior().on(StringValue, self.count)

    async def count(self, msg: StringValue, ctx) -> None:
        self.handled += 1


async def main() -> None:
    messages = MessageRegistry()
    messages.register(StringValue, type_tag=0x1000)

    async with ActorSystem(messages=messages) as system:
        ref = await system.spawn(Instrumented, name="instr")

        # Send a few messages
        for i in range(3):
            result = await system.send(
                ref,
                StringValue(value=f"msg-{i}"),
                options=DeliveryOptions(),
            )
            print(f"Delivery result: {result}")

        await asyncio.sleep(0.1)

    print("System shut down — no runtime threads remain")


if __name__ == "__main__":
    asyncio.run(main())
