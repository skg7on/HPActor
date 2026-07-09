#!/usr/bin/env python3
"""Five-minute echo actor — the simplest HPActor Python example.

Uses an explicit protobuf TypeTag (0x1000) for StringValue messages.
"""

import asyncio

from google.protobuf.wrappers_pb2 import StringValue

from hpactor import Actor, ActorSystem, Behavior, MessageRegistry


class Echo(Actor):
    def behavior(self) -> Behavior:
        return Behavior().on_request(StringValue, StringValue, self.echo)

    async def echo(self, msg: StringValue, ctx) -> StringValue:
        return StringValue(value=msg.value)


async def main() -> None:
    messages = MessageRegistry()
    messages.register(StringValue, type_tag=0x1000)

    async with ActorSystem(messages=messages) as system:
        ref = await system.spawn(Echo, name="echo")
        reply = await system.ask(
            ref, StringValue(value="hello"),
            response_type=StringValue, timeout=5.0,
        )
        print(f"Echo replied: {reply.value}")


if __name__ == "__main__":
    asyncio.run(main())
