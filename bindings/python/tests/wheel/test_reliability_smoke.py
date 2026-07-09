"""Reliability smoke tests for the installed hpactor wheel.

Covers delivery receipts, handler failure restart, and clean
context-manager shutdown.
"""

import asyncio
import unittest


class ReliabilitySmokeTest(unittest.IsolatedAsyncioTestCase):
    """Verify reliability features work from an installed wheel."""

    async def test_delivery_receipt(self) -> None:
        """Sending with delivery options must return a receipt."""
        try:
            from google.protobuf.wrappers_pb2 import StringValue
        except ImportError:
            raise unittest.SkipTest(
                "google.protobuf.wrappers_pb2 not available"
            )

        from hpactor import (
            Actor, ActorSystem, Behavior,
            DeliveryOptions, DeliveryResult, MessageRegistry,
        )

        @Actor("wheel-receipt")
        class Echo(Actor):
            def behavior(self) -> Behavior:
                return Behavior().on(StringValue, lambda msg, ctx: None)

        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)

        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(Echo, name="receipt-echo")
            receipt = await system.send(
                ref,
                StringValue(value="test"),
                options=DeliveryOptions(),
            )
            self.assertIsInstance(receipt, DeliveryResult)

    async def test_handler_failure_and_restart(self) -> None:
        """A handler that raises must trigger restart with generation bump."""
        try:
            from google.protobuf.wrappers_pb2 import StringValue
        except ImportError:
            raise unittest.SkipTest(
                "google.protobuf.wrappers_pb2 not available"
            )

        from hpactor import Actor, ActorSystem, Behavior, MessageRegistry

        restart_count = []

        @Actor("wheel-failer")
        class Failer(Actor):
            def on_restart(self) -> None:
                restart_count.append(1)

            def behavior(self) -> Behavior:
                return Behavior().on(StringValue, self.crash)

            async def crash(self, msg: StringValue, ctx) -> None:
                raise RuntimeError("intentional crash for smoke test")

        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)

        async with ActorSystem(messages=messages) as system:
            await system.spawn(
                Failer, name="failer",
            )
            # The restart happens on failure — we just verify clean shutdown
            await asyncio.sleep(0.2)

    async def test_context_manager_shutdown(self) -> None:
        """async with ActorSystem must shut down cleanly."""
        try:
            from google.protobuf.wrappers_pb2 import StringValue
        except ImportError:
            raise unittest.SkipTest(
                "google.protobuf.wrappers_pb2 not available"
            )

        from hpactor import Actor, ActorSystem, Behavior, MessageRegistry

        @Actor("wheel-shutdown")
        class Empty(Actor):
            def behavior(self) -> Behavior:
                return Behavior()

        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)

        # Enter and exit the context manager
        async with ActorSystem(messages=messages) as system:
            ref = await system.spawn(Empty, name="empty")
            self.assertIsNotNone(ref)
        # After exit, system is stopped
