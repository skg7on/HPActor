# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Tests for PythonTopologyPolicy allowlist and fingerprint."""

import unittest

from hpactor._topology import PythonTopologyPolicy, _is_absolute_module_name


class TopologyPolicyTest(unittest.TestCase):
    """Tests for allowlist matching and policy validation."""

    def test_prefix_matches_exact_name(self) -> None:
        policy = PythonTopologyPolicy(("topology_app.actors",))
        self.assertTrue(policy.allows("topology_app.actors"))

    def test_prefix_matches_dotted_child(self) -> None:
        policy = PythonTopologyPolicy(("topology_app.actors",))
        self.assertTrue(policy.allows("topology_app.actors.billing"))

    def test_prefix_rejects_lookalike(self) -> None:
        policy = PythonTopologyPolicy(("topology_app.actors",))
        self.assertFalse(policy.allows("topology_app.actors_evil"))

    def test_prefix_rejects_unrelated(self) -> None:
        policy = PythonTopologyPolicy(("my_app",))
        self.assertFalse(policy.allows("other_app.actors"))

    def test_empty_policy_raises(self) -> None:
        with self.assertRaises(ValueError):
            PythonTopologyPolicy(())

    def test_invalid_module_name_raises(self) -> None:
        with self.assertRaises(ValueError):
            PythonTopologyPolicy(("not a valid module",))

    def test_relative_import_rejected(self) -> None:
        self.assertFalse(_is_absolute_module_name(".hidden"))

    def test_leading_digit_rejected(self) -> None:
        self.assertFalse(_is_absolute_module_name("123module"))

    def test_space_rejected(self) -> None:
        self.assertFalse(_is_absolute_module_name("my module"))

    def test_fingerprint_is_deterministic(self) -> None:
        p1 = PythonTopologyPolicy(("a.b", "c.d"))
        p2 = PythonTopologyPolicy(("a.b", "c.d"))
        self.assertEqual(p1.fingerprint, p2.fingerprint)

    def test_fingerprint_changes_with_content(self) -> None:
        p1 = PythonTopologyPolicy(("a.b",))
        p2 = PythonTopologyPolicy(("a.b", "c.d"))
        self.assertNotEqual(p1.fingerprint, p2.fingerprint)

    def test_fingerprint_is_order_independent(self) -> None:
        p1 = PythonTopologyPolicy(("b.c", "a.d"))
        p2 = PythonTopologyPolicy(("a.d", "b.c"))
        self.assertEqual(p1.fingerprint, p2.fingerprint)

    def test_duplicate_prefix_deduplicated(self) -> None:
        p = PythonTopologyPolicy(("x.y", "x.y"))
        self.assertEqual(len(p.allowed_module_prefixes), 1)


if __name__ == "__main__":
    unittest.main()
