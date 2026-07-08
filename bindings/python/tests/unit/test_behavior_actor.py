# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Unit tests for Behavior, Actor, and ActorContext."""

import asyncio
import unittest

from google.protobuf.wrappers_pb2 import StringValue

from hpactor import (
    Actor,
    ActorContext,
    ActorNotReadyError,
    Behavior,
    MessageRegistry,
    actor,
)


class EchoActor(Actor):
    async def echo(self, msg: StringValue, ctx: ActorContext) -> StringValue:
        return StringValue(value=msg.value)

    def behavior(self) -> Behavior:
        return Behavior().on_request(StringValue, StringValue, self.echo)


class BehaviorTest(unittest.TestCase):
    def test_build_and_freeze(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()

        b = Behavior().on_request(StringValue, StringValue,
                                   lambda m, c: m)
        self.assertFalse(b.frozen)
        b.freeze(registry)
        self.assertTrue(b.frozen)

        # Freeze after freeze raises.
        with self.assertRaises(ActorNotReadyError):
            b.on(StringValue, lambda m, c: None)

        entry = b.handler_for(StringValue)
        self.assertIsNotNone(entry)

    def test_mutation_after_freeze_raises(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()

        b = Behavior().on(StringValue, lambda m, c: None)
        b.freeze(registry)
        with self.assertRaises(ActorNotReadyError):
            b.on_request(StringValue, StringValue, lambda m, c: m)


class ActorDecoratorTest(unittest.TestCase):
    def test_decorator_sets_name(self) -> None:
        @actor("echo")
        class Echo(Actor):
            def behavior(self) -> Behavior:
                return Behavior()

        self.assertEqual(Echo.__hpactor_actor_name__, "echo")

    def test_decorator_rejects_empty(self) -> None:
        with self.assertRaises(ValueError):
            @actor("")
            class Bad(Actor):
                def behavior(self) -> Behavior:
                    return Behavior()


class ActorTest(unittest.IsolatedAsyncioTestCase):
    async def test_become_requires_bound(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()

        inst = EchoActor()
        b = Behavior().on(StringValue, lambda m, c: None)
        b.freeze(registry)
        with self.assertRaises(ActorNotReadyError):
            inst.become(b)

    async def test_bind_freezes_behavior(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()

        inst = EchoActor()
        inst._bind(registry)
        self.assertIsNotNone(inst.current_behavior)
        self.assertTrue(inst.current_behavior.frozen)


if __name__ == "__main__":
    unittest.main()
