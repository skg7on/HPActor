Your First Python Actor
=======================

This guide walks through defining, spawning, and messaging a Python actor
using the HPActor in-process API.

Define an Actor
---------------

.. code-block:: python

    from hpactor import Actor, Behavior, actor

    @actor("greeter")
    class GreeterActor(Actor):
        def __init__(self, greeting: str = "Hello"):
            super().__init__()
            self._greeting = greeting

        async def on_start(self) -> None:
            print(f"GreeterActor started with greeting={self._greeting!r}")

        def behavior(self) -> Behavior:
            b = Behavior()
            b.on("GreetRequest", self._on_greet)
            return b

        async def _on_greet(self, msg, ctx):
            print(f"{self._greeting}, {msg.name}!")

Actors subclass ``Actor`` and are decorated with ``@actor("name")``.
Each actor must implement ``behavior()`` which returns a ``Behavior``
with registered message handlers.  The optional ``on_start()`` hook
runs after the actor is published.

Start an ActorSystem
--------------------

.. code-block:: python

    import asyncio
    from hpactor import ActorSystem, DeliveryOptions, MessageRegistry

    async def main():
        registry = MessageRegistry()
        registry.freeze()

        system = ActorSystem(config={"scheduler_threads": 1})

        async with system:
            greeter = await system.spawn(
                GreeterActor,
                args={"greeting": "Hi"},
                messages=registry,
            )

            # Send a fire-and-forget message
            msg = GreetRequestProto(name="World")
            await system.send(greeter, msg)

            # Ask for a reply (request-response with timeout)
            reply = await system.ask(greeter, msg, timeout=5.0)
            print(f"Reply: {reply}")

    asyncio.run(main())

``ActorSystem`` is an async context manager.  ``__aenter__`` starts the
native runtime; ``__aexit__`` drains actors and stops threads.  All
actor operations — spawn, send, ask, schedule — are ``await``\ ed.

Message Types
-------------

HPActor uses protobuf for messages. Each message type must have an
explicit ``TypeTag`` registered in a ``MessageRegistry``:

.. code-block:: python

    from hpactor import MessageRegistry

    registry = MessageRegistry()
    registry.register(0x1001, GreetRequestProto)
    registry.register(0x1002, GreetResponseProto)
    registry.freeze()  # no further registrations

The registry must be frozen before use in ``spawn()`` or
``from_topology()``.

Lifecycle Hooks
---------------

+------------------+---------------------------------------------+
| Hook             | When                                        |
+==================+=============================================+
| ``on_start()``   | After the actor is published (async)        |
+------------------+---------------------------------------------+
| ``on_stop()``    | During graceful drain (async)               |
+------------------+---------------------------------------------+
| ``on_restart()`` | Before a new incarnation starts (async)      |
+------------------+---------------------------------------------+

The actor receives structured messages for linked exit, monitor down,
and restart notifications via the dispatch pipeline.

Declarative Topology
--------------------

Python actors can also be declared in TOML topology files alongside
C++ actors.  See :doc:`topology` for the full declarative bootstrap
guide.
