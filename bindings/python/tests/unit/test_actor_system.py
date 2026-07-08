# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Unit tests for ActorSystem lifecycle and spawn."""

import asyncio
import unittest

from google.protobuf.wrappers_pb2 import StringValue

from hpactor import (
    Actor,
    ActorContext,
    ActorNotReadyError,
    ActorSystem,
    Behavior,
    DeliveryStatus,
    MessageRegistry,
    SystemClosedError,
    actor,
)


@actor("echo")
class EchoActor(Actor):
    started = 0
    stopped = 0

    async def on_start(self) -> None:
        type(self).started += 1

    async def on_stop(self) -> None:
        type(self).stopped += 1

    def behavior(self) -> Behavior:
        return Behavior().on_request(StringValue, StringValue, self.echo)

    async def echo(self, msg: StringValue, ctx: ActorContext) -> StringValue:
        return msg


class ActorSystemTest(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        EchoActor.started = 0
        EchoActor.stopped = 0

    async def test_context_manager_spawns_and_stops_actor(self) -> None:
        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)
        messages.freeze()

        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(EchoActor, name="echo")
            self.assertEqual(ref.name, "echo")
            self.assertEqual(EchoActor.started, 1)
            self.assertEqual(EchoActor.stopped, 0)
        self.assertEqual(EchoActor.stopped, 1)

    async def test_rejects_use_after_close(self) -> None:
        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)
        messages.freeze()

        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(EchoActor, name="echo")

        with self.assertRaises(SystemClosedError):
            system.send(ref, StringValue(value="x"))

    async def test_send_returns_accepted_receipt(self) -> None:
        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)
        messages.freeze()

        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(EchoActor, name="echo")
            receipt = system.send(ref, StringValue(value="hello"))
            self.assertTrue(receipt.done())
            result = receipt.result()
            self.assertEqual(result.status, DeliveryStatus.Accepted)

    async def test_requires_frozen_registry(self) -> None:
        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)
        # Not frozen yet — should raise.
        with self.assertRaises(ActorNotReadyError):
            async with ActorSystem(messages=messages):
                pass


if __name__ == "__main__":
    unittest.main()
