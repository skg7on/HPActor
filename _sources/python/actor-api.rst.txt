Actor API Reference
===================

This page covers the imperative Python actor programming model —
defining actors, sending messages, asking for replies, and using
delivery options.

ActorSystem
-----------

.. py:class:: ActorSystem

   The main entry point for the in-process actor runtime.

   .. py:classmethod:: from_topology(path, *, messages, policy, config=None)

      Construct a system for declarative TOML topology bootstrap.
      Side-effect free — no threads or native state until ``__aenter__``.

   .. py:method:: async spawn(actor_class, *, args=None, messages, delivery=None)

      Spawn a new actor.  Returns an ``ActorAddress``.

      :param type actor_class: An ``Actor`` subclass decorated with ``@actor()``.
      :param dict args: Keyword arguments forwarded to the constructor.
      :param MessageRegistry messages: Frozen protobuf message registry.
      :param DeliveryOptions delivery: Optional delivery mode and deadline.

   .. py:method:: async send(target, message, *, options=None)

      Send a fire-and-forget message to *target*.

   .. py:method:: async ask(target, message, *, timeout=5.0, options=None)

      Send a request and await a typed response.  Raises
      ``AskTimeoutError`` if no response arrives within *timeout* seconds.

   .. py:method:: resolve(name)

      Look up a named actor.  Returns an ``ActorAddress`` or raises ``KeyError``.

   .. py:method:: async schedule(target, delay, message)

      Deliver *message* to *target* after *delay* seconds.

   .. py:method:: async __aenter__()

      Start the native runtime.  All actor state is initialized.

   .. py:method:: async __aexit__(*args)

      Initiate graceful drain, stop all actors, join threads.

Actor
-----

.. py:class:: Actor

   Base class for all Python actors.

   .. py:method:: behavior() -> Behavior

      Return a ``Behavior`` with registered message handlers.
      Called once at startup.  The behavior is frozen after construction.

   .. py:method:: async on_start()

      Called after the actor is published and the system is running.

   .. py:method:: async on_stop()

      Called during graceful shutdown before the actor is removed.

   .. py:method:: async on_restart()

      Called before a new incarnation, if supervision restarts the actor.

Behavior
--------

.. py:class:: Behavior

   Immutable message handler table.

   .. py:method:: on(message_type, handler)

      Register *handler* for a protobuf message class.

      Handler signature: ``async def handler(msg, ctx)`` where *msg* is
      the deserialized protobuf message and *ctx* is an ``ActorContext``.

   .. py:method:: on_request(request_type, response_type, handler)

      Register a request-response handler.  Handler must return a
      protobuf response message or raise an error.

ActorContext
------------

.. py:class:: ActorContext

   Handler-scoped context providing message-passing primitives.

   .. py:method:: async reply(message)

      Reply to the current sender.

   .. py:method:: async reply_error(code, detail="")

      Reply with a structured error.

   .. py:method:: async send(target, message, *, options=None)

      Send a message to another actor.

   .. py:method:: async spawn(actor_class, *, args=None, messages)

      Spawn a child actor.

   .. py:method:: async schedule(delay, message)

      Schedule a self-delivery after *delay* seconds.

   .. py:attribute:: sender

      The ``ActorAddress`` of the current message sender (read-only).

   .. py:attribute:: self_address

      This actor's own ``ActorAddress`` (read-only).

Delivery
--------

.. py:class:: DeliveryOptions

   Configure delivery semantics for a send or ask.

   .. py:attribute:: mode
      :type: DeliveryMode

      One of ``BEST_EFFORT``, ``OBSERVABLE``, ``AT_LEAST_ONCE``,
      or ``DURABLE_AT_LEAST_ONCE``.

   .. py:attribute:: deadline_ms

      Delivery deadline in milliseconds.  Messages not delivered before
      the deadline are dropped and recorded.

   .. py:attribute:: no_drop

      If ``True``, the mailbox will reject the enqueue instead of
      dropping when full.

.. py:class:: DeliveryReceipt

   Returned from ``send()`` with *observable* delivery mode.

   .. py:attribute:: status
      :type: DeliveryStatus

   .. py:attribute:: message_id

.. py:class:: DeliveryStatus

   Enum: ``ACCEPTED``, ``REJECTED``, ``DROPPED``, ``DEAD_LETTER``,
   ``EXPIRED``, ``CIRCUIT_OPEN``, ``QUARANTINED``.

Addresses
---------

.. py:class:: ActorAddress

   Opaque identifier for an actor.  Constructed internally; user code
   receives addresses from ``spawn()``, ``resolve()``, or ``ctx.self_address``.

   .. py:attribute:: actor_id
      :type: int

   .. py:attribute:: incarnation
      :type: int

.. py:class:: ActorRef

   Unified reference: a local ``ActorAddress`` or a remote proxy.
   Used as the target for ``send()`` and ``ask()``.

Errors
------

All binding errors inherit from ``HPActorError``:

- ``ActorError`` — an actor replied with an explicit error
- ``ActorNotReadyError`` — operation on an actor not yet started
- ``AskTimeoutError`` — ``ask()`` timed out
- ``AskCancelledError`` — ``ask()`` was cancelled
- ``RegistrationError`` — protobuf registry conflict
- ``SerializationError`` — protobuf encode/decode failure
- ``ResourceExhaustedError`` — queue or lease pool exhausted
- ``SystemClosedError`` — operation after system shutdown
- ``TopologyError`` — declarative topology failure
