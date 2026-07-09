API Reference
=============

This page lists every public symbol in ``hpactor.__all__``.

Actor System
------------

``ActorSystem(messages, config=None)``
    Async context manager.  Starts and stops the native runtime.

``system.spawn(actor_cls, name, **kwargs) → ActorRef``
    Spawn a new actor instance.

``system.send(ref, msg, options=None) → DeliveryResult``
    Send a fire-and-forget message.

``system.ask(ref, msg, response_type, timeout=5.0) → Message``
    Send a request and await the response.

Actor
-----

``Actor`` (base class)
    Subclass and override ``behavior()``.

``actor(name)`` (decorator)
    Declare an actor class with a name.

Behavior
--------

``Behavior()``
    Immutable handler table.

``.on(MessageCls, handler)``
    Fire-and-forget handler.

``.on_request(ReqCls, ResCls, handler)``
    Request-response handler.

ActorContext
------------

``ctx.reply(msg)``
    Reply to the current request sender.

``ctx.reply_with_error(code)``
    Reply with an error code.

``ctx.schedule(delay, msg) → ScheduleHandle``
    Schedule self-delivery after ``delay`` seconds.

``ctx.cancel_schedule(handle)``
    Cancel a pending scheduled message.

Delivery
--------

``DeliveryMode`` (enum)
    BestEffort, ObservableBestEffort, AtLeastOnce, DurableAtLeastOnce.

``DeliveryOptions``
    Configure delivery mode, deadline, and dedup settings.

``DeliveryReceipt``
    Receipt for a sent message with completion callback support.

``DeliveryResult``
    Result of a send operation.

``DeliveryStatus`` (enum)
    Success, Rejected, Timeout, etc.

``FailureReason`` (enum)
    23 values in 10 semantic ranges.

``FailureSource`` (enum)
    12 subsystem origins.

Errors
------

All exceptions inherit from ``HPActorError``:

- ``ActorError``
- ``ActorNotReadyError``
- ``AskCancelledError``
- ``AskTimeoutError``
- ``RegistrationError``
- ``ResourceExhaustedError``
- ``SerializationError``
- ``SystemClosedError``

Other
-----

``MessageRegistry``
    Register protobuf message types with explicit TypeTags.

``ActorAddress``
    Opaque actor address.

``ActorRef``
    Reference to a spawned actor.

``ScheduleHandle``
    Handle for a scheduled message.
