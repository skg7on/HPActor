# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Unit tests for address, delivery, and enum types."""

import unittest

from hpactor import (
    ActorAddress,
    DeliveryMode,
    DeliveryOptions,
    DeliveryReceipt,
    DeliveryResult,
    DeliveryStatus,
    FailureReason,
    FailureSource,
)


class AddressTest(unittest.TestCase):
    def test_valid_addresses(self) -> None:
        a = ActorAddress(family=0, packed_address=b"", port=0,
                         actor_type=1, actor_id=42, incarnation=1)
        self.assertEqual(a.family, 0)
        self.assertEqual(a.actor_id, 42)

    def test_rejects_invalid_family(self) -> None:
        with self.assertRaises(ValueError):
            ActorAddress(family=1, packed_address=b"", port=0,
                         actor_type=0, actor_id=0, incarnation=0)

    def test_rejects_wrong_packed_length(self) -> None:
        with self.assertRaises(ValueError):
            ActorAddress(family=4, packed_address=b"ab", port=0,
                         actor_type=0, actor_id=0, incarnation=0)


class DeliveryTest(unittest.TestCase):
    def test_delivery_result_properties(self) -> None:
        r = DeliveryResult(status=DeliveryStatus.Accepted)
        self.assertTrue(r.accepted)
        self.assertFalse(r.retryable)

        r2 = DeliveryResult(status=DeliveryStatus.NoRoute)
        self.assertFalse(r2.accepted)
        self.assertTrue(r2.retryable)

    def test_failure_reason_mapping(self) -> None:
        r = DeliveryResult(status=DeliveryStatus.NoRoute)
        self.assertEqual(r.failure_reason, FailureReason.NoRoute)

    def test_delivery_options_validation(self) -> None:
        opts = DeliveryOptions(priority=0)
        self.assertEqual(opts.priority, 0)
        with self.assertRaises(ValueError):
            DeliveryOptions(priority=4)

    def test_delivery_receipt_awaitable(self) -> None:
        import asyncio
        receipt = DeliveryReceipt()
        self.assertFalse(receipt.done())

        async def resolve() -> DeliveryResult:
            receipt._resolve(DeliveryResult(status=DeliveryStatus.Accepted))
            return await receipt

        result = asyncio.new_event_loop().run_until_complete(resolve())
        self.assertTrue(result.accepted)

    def test_enum_values_match_contract(self) -> None:
        self.assertEqual(DeliveryMode.BestEffort, 0)
        self.assertEqual(DeliveryMode.AtLeastOnce, 2)
        self.assertEqual(DeliveryStatus.Accepted, 0)
        self.assertEqual(DeliveryStatus.Cancelled, 12)
        self.assertEqual(FailureReason.Unknown, 255)
        self.assertEqual(FailureSource.LanguageBinding, 12)


if __name__ == "__main__":
    unittest.main()
