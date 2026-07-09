Your First Actor
================

This guide walks through defining, spawning, and messaging your first
HPActor Python actor.

Define an Actor
---------------

.. code-block:: python

   from google.protobuf.wrappers_pb2 import StringValue
   from hpactor import Actor, Behavior

   class Echo(Actor):
       def behavior(self) -> Behavior:
           return Behavior().on_request(
               StringValue, StringValue, self.echo)

       async def echo(self, msg: StringValue, ctx) -> StringValue:
           return StringValue(value=msg.value)

Register Messages
-----------------

Every protobuf message type used by actors must be registered with an
explicit ``TypeTag``:

.. code-block:: python

   from hpactor import MessageRegistry

   messages = MessageRegistry()
   messages.register(StringValue, type_tag=0x1000)

Start the System
----------------

.. code-block:: python

   from hpactor import ActorSystem

   async with ActorSystem(messages=messages) as system:
       ref = await system.spawn(Echo, name="echo")
       reply = await system.ask(
           ref, StringValue(value="hello"),
           response_type=StringValue, timeout=5.0)
       print(reply.value)

The ``async with`` context manager starts and stops the actor system
automatically.  After exit, all actors are drained and no runtime threads
remain.

Complete Example
----------------

See ``bindings/python/examples/echo.py`` for the complete runnable example.
