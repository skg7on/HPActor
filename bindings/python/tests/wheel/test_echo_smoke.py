"""End-to-end smoke test for an installed hpactor wheel.

Spawns an echo actor, sends a request, verifies the response, and
shuts down cleanly.  Requires protobuf StringValue.
"""

import asyncio
import unittest


class WheelEchoTest(unittest.IsolatedAsyncioTestCase):
    """Verify installed-wheel echo and ask workflow."""

    async def test_echo_ask_and_shutdown(self) -> None:
        """Spawn an echo actor, send a request, verify response, shutdown."""
        try:
            from google.protobuf.wrappers_pb2 import StringValue
        except ImportError:
            raise unittest.SkipTest(
                "google.protobuf.wrappers_pb2 not available"
            )

        from hpactor import Actor, ActorSystem, Behavior, MessageRegistry

        @Actor("wheel-echo")
        class Echo(Actor):
            def behavior(self) -> Behavior:
                return Behavior().on_request(
                    StringValue, StringValue, self.echo)

            async def echo(self, message: StringValue,
                           ctx) -> StringValue:
                return StringValue(value=message.value)

        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)

        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(Echo, name="echo")
            reply = await system.ask(
                ref,
                StringValue(value="wheel"),
                response_type=StringValue,
                timeout=5.0,
            )
            self.assertEqual(reply.value, "wheel")

    async def test_fire_and_forget(self) -> None:
        """Fire-and-forget send must not error."""
        try:
            from google.protobuf.wrappers_pb2 import StringValue
        except ImportError:
            raise unittest.SkipTest(
                "google.protobuf.wrappers_pb2 not available"
            )

        from hpactor import Actor, ActorSystem, Behavior, MessageRegistry

        received = []

        @Actor("wheel-ff")
        class FFActor(Actor):
            def behavior(self) -> Behavior:
                return Behavior().on(StringValue, self.handle)

            async def handle(self, msg: StringValue, ctx) -> None:
                received.append(msg.value)

        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)

        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(FFActor, name="ff")
            await system.send(ref, StringValue(value="hello"))
            # Give the message time to be delivered
            await asyncio.sleep(0.1)

        self.assertIn("hello", received)
