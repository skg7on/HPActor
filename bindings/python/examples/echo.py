#!/usr/bin/env python3
"""echo.py — Five-minute first actor with hpactor.

Demonstrates imperative spawn, ask/reply, and clean shutdown.
Uses generated protobuf StringValue with an explicit, stable TypeTag.
"""

import asyncio

import hpactor
from google.protobuf.wrappers_pb2 import StringValue


@hpactor.actor("echo")
class Echo(hpactor.Actor):
    def behavior(self) -> hpactor.Behavior:
        return hpactor.Behavior().on_request(
            StringValue, StringValue, self.echo
        )

    async def echo(
        self, msg: StringValue, ctx: hpactor.ActorContext
    ) -> StringValue:
        return StringValue(value=msg.value)


async def main() -> None:
    messages = hpactor.MessageRegistry()
    messages.register(StringValue, type_tag=0x1000)

    async with hpactor.ActorSystem(messages=messages) as system:
        echo = await system.spawn(Echo, name="echo")
        reply = await system.ask(
            echo,
            StringValue(value="hello, hpactor!"),
            response_type=StringValue,
            timeout=5.0,
        )
        print(f"Reply: {reply.value}")
        assert reply.value == "hello, hpactor!"


if __name__ == "__main__":
    asyncio.run(main())
