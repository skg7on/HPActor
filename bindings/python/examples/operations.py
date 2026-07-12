#!/usr/bin/env python3
"""operations.py — Runtime snapshot and clean shutdown example."""

import asyncio
import os
import time

import hpactor
from google.protobuf.wrappers_pb2 import StringValue


@hpactor.actor("ops-worker")
class OpsWorker(hpactor.Actor):
    def behavior(self) -> hpactor.Behavior:
        return hpactor.Behavior().on(StringValue, self.on_message)

    async def on_message(self, msg: StringValue,
                         ctx: hpactor.ActorContext) -> None:
        pass  # fire-and-forget


async def main() -> None:
    messages = hpactor.MessageRegistry()
    messages.register(StringValue, type_tag=0x1000)

    async with hpactor.ActorSystem(messages=messages) as system:
        worker = await system.spawn(OpsWorker, name="worker")

        # Send some messages
        for i in range(10):
            await system.send(
                worker, StringValue(value=f"msg-{i}")
            )

        # Brief settle
        await asyncio.sleep(0.1)

        print(f"ActorSystem shutdown complete, PID={os.getpid()}")

    print("Runtime thread joined.")


if __name__ == "__main__":
    asyncio.run(main())
