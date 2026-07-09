Message Passing
===============

HPActor Python actors communicate through protobuf messages with explicit
``TypeTag`` values.

Message Registration
--------------------

Every protobuf message type used across actors must be registered before
use:

.. code-block:: python

   from hpactor import MessageRegistry

   messages = MessageRegistry()
   messages.register(StringValue, type_tag=0x1000)

The ``TypeTag`` must be a 16-bit integer in the application range
(``0x1000`` to ``0x00FFFFFF``).  Tags ``0x00``--``0x0F`` are reserved
for system messages.

Fire and Forget
---------------

.. code-block:: python

   await system.send(ref, StringValue(value="fire-and-forget"))

Request-Response
----------------

.. code-block:: python

   reply = await system.ask(
       ref,
       StringValue(value="request"),
       response_type=StringValue,
       timeout=5.0,
   )

The ``timeout`` parameter specifies the maximum wait time in seconds.
If the timeout expires, ``AskTimeoutError`` is raised.

Delivery Options
----------------

.. code-block:: python

   from hpactor import DeliveryOptions

   result = await system.send(
       ref, msg,
       options=DeliveryOptions(),
   )

Scheduling
----------

.. code-block:: python

   handle = await context.schedule(
       delay=1.0,
       message=StringValue(value="delayed"),
   )
   # Cancel before delivery:
   await context.cancel_schedule(handle)

Error Handling
--------------

Handler exceptions are propagated as ``ActorError`` to the caller of
``ask()``.  The failing actor is automatically restarted by its
supervisor with a new generation.
