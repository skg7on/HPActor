#!/usr/bin/env python3
"""Typed request-response example with error handling.

Demonstrates ask/reply with protobuf messages, ActorError handling,
and explicit TypeTags.
"""

import asyncio

from google.protobuf.wrappers_pb2 import Int32Value, StringValue

from hpactor import (
    Actor, ActorError, ActorSystem, Behavior, MessageRegistry,
)


class Calculator(Actor):
    def behavior(self) -> Behavior:
        return Behavior() \
            .on_request(Int32Value, Int32Value, self.double) \
            .on_request(StringValue, StringValue, self.greet)

    async def double(self, msg: Int32Value, ctx) -> Int32Value:
        return Int32Value(value=msg.value * 2)

    async def greet(self, msg: StringValue, ctx) -> StringValue:
        if not msg.value:
            raise ValueError("empty name")
        return StringValue(value=f"Hello, {msg.value}!")


async def main() -> None:
    messages = MessageRegistry()
    messages.register(Int32Value, type_tag=0x1001)
    messages.register(StringValue, type_tag=0x1000)

    async with ActorSystem(messages=messages) as system:
        calc = await system.spawn(Calculator, name="calc")

        # Successful request
        result = await system.ask(
            calc, Int32Value(value=21),
            response_type=Int32Value, timeout=5.0,
        )
        print(f"21 * 2 = {result.value}")

        # Request that triggers an error
        try:
            await system.ask(
                calc, StringValue(value=""),
                response_type=StringValue, timeout=5.0,
            )
        except ActorError as exc:
            print(f"Expected error: {exc}")


if __name__ == "__main__":
    asyncio.run(main())
