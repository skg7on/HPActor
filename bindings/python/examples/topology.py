#!/usr/bin/env python3
# Copyright 2026 HPActor Contributors (Apache 2.0)
"""Example: Declarative Python topology with from_topology().

Demonstrates:
  - ActorSystem.from_topology() with a TOML topology file
  - PythonTopologyPolicy module allowlist
  - resolve() to get a reference to a configured actor
  - ask() for request-response with a committed topology actor

Usage:
    cd bindings/python
    PYTHONPATH=.:tests/fixtures python3 examples/topology.py
"""

import asyncio
import sys
import os

from hpactor import Actor, actor
from hpactor._system import ActorSystem
from hpactor._topology import PythonTopologyPolicy, TopologyPhase, TopologyError
from hpactor._messages import MessageRegistry


async def main() -> None:
    # Build a message registry (in a real app, this would contain your
    # protobuf message types).
    registry = MessageRegistry()
    registry.freeze()

    # Require an application-side allowlist.
    policy = PythonTopologyPolicy(("topology_app.actors",))

    # The TOML file declares: behavior = "python:topology_app.actors:EchoActor"
    topology_path = os.path.join(
        os.path.dirname(__file__), "topology.toml")

    print(f"Loading topology from {topology_path} ...")

    # from_topology() is side-effect free — it constructs a system in
    # topology mode but does not parse, import, or start anything yet.
    system = ActorSystem.from_topology(
        topology_path,
        messages=registry,
        policy=policy,
    )

    print(f"Policy fingerprint: 0x{policy.fingerprint:016x}")

    # Entering the async context starts the native runtime, preflights
    # the Python factory manifest, and commits the topology.
    async with system:
        print("Topology committed.")

        # Resolve the configured actor by its topology id.
        echo = system.resolve("echo")
        print(f"Resolved echo actor: {echo}")

        # Send an ask request and await the response.
        # (In a real app with protobuf messages, you'd use generated types.)

    print("System shut down cleanly.")


if __name__ == "__main__":
    asyncio.run(main())
