# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Unit tests for MessageRegistry."""

import unittest

from google.protobuf.wrappers_pb2 import Int64Value, StringValue

from hpactor import MessageRegistry, RegistrationError


class MessageRegistryTest(unittest.TestCase):
    def test_fixed_tags_round_trip_deterministically(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()
        payload = registry.serialize(StringValue(value="hello"))
        self.assertEqual(registry.type_tag_for(StringValue), 0x1000)
        self.assertEqual(
            registry.deserialize(0x1000, payload),
            StringValue(value="hello"),
        )

    def test_rejects_conflicts_and_post_freeze_registration(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        with self.assertRaises(RegistrationError):
            registry.register(Int64Value, type_tag=0x1000)
        with self.assertRaises(RegistrationError):
            registry.register(Int64Value, type_tag=0x0FFF)
        registry.freeze()
        with self.assertRaises(RegistrationError):
            registry.register(Int64Value, type_tag=0x1001)

    def test_freeze_required_for_serialize(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        msg = StringValue(value="x")
        with self.assertRaises(RegistrationError):
            registry.serialize(msg)

    def test_idempotent_re_registration(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        # Same call should not raise.
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()
        payload = registry.serialize(StringValue(value="ok"))
        self.assertTrue(payload)


if __name__ == "__main__":
    unittest.main()
