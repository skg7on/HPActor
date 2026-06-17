.. _building-applications-message-passing:

Message Passing
===============

Communication in HPActor is exclusively through messages. This chapter
covers the message types, handlers, and patterns used to build actor
interactions.

Message Model
-------------

HPActor uses Protocol Buffers for wire-compatible messages, wrapped in a
:cpp:class:`TypedMessage<T>` that carries metadata:

.. code-block:: cpp

   template <typename T>
   struct TypedMessage {
       TypeTag tag;         // Identifies the message type for dispatch
       T payload;           // The protobuf message
       ActorId sender;      // Who sent it (for reply routing)
       MessageId message_id; // Unique message identifier
   };

Fire-and-Forget Handlers
------------------------

Register a handler that processes a message without sending a response:

.. code-block:: cpp

   Behavior make_behavior() override {
       return Behavior::make()
           .on<LogEntry>([this](const LogEntry& entry) {
               buffer_.push_back(entry);
           })
           .on<Flush>([this](const Flush&) {
               write_buffer_to_disk();
               buffer_.clear();
           });
   }

The ``on<T>(handler)`` registration:
- Takes a callable with signature ``void(const T&)``.
- The actor does not send a reply to the sender.
- Automatically deserializes the protobuf payload.

Request-Response Handlers
-------------------------

Use ``on_request<ReqT, ResT>(handler)`` when the actor should reply:

.. code-block:: cpp

   Behavior make_behavior() override {
       return Behavior::make()
           .on_request<QueryRequest, QueryResponse>(
               [this](const QueryRequest& req) {
                   QueryResponse resp;
                   resp.set_result(lookup(req.key()));
                   context()->reply(resp);
               });
   }

The framework:
- Deserializes the ``QueryRequest`` from the incoming message.
- Calls the handler.
- Routes the reply back to the original sender when ``context()->reply()`` is used.

Sending Messages
----------------

From within an actor (via :cpp:class:`ActorContext`):

.. code-block:: cpp

   // Send to a known ActorRef
   context()->send(target_ref, message);

   // Reply to the current sender (set per-message by the framework)
   context()->reply(response);

   // Reply with an error code
   context()->reply_with_error(error::timeout);

   // Send to a raw ActorId
   context()->send(target_id, message);

From outside the actor system (via :cpp:class:`ScopedActor`):

.. code-block:: cpp

   ScopedActor client(system);
   client.send(actor_ref, message);
   auto response = client.receive<ResponseType>();

The Ask Pattern (Request-Future)
--------------------------------

For request-response from non-actor contexts or across subsystems, use
the ask pattern via :cpp:class:`AskManager`:

.. code-block:: cpp

   // From within an actor:
   auto handle = context()->ask<QueryResponse>(target, QueryRequest{...});
   // ... do other work ...
   if (handle.ready()) {
       auto result = handle.get();  // blocks until response or timeout
   }

The ask subsystem:
- Generates a unique ``MessageId`` for correlation.
- Schedules a timeout timer (configurable via ``[system.ask]`` in TOML).
- Resolves the handle when the response arrives or the deadline expires.
- On timeout, emits a :cpp:enum:`DeadLetterReason::AskTimeout` record.

Delivery Modes
--------------

HPActor supports configurable delivery semantics per send:

.. list-table::
   :header-rows: 1

   * - Mode
     - Guarantee
     - Use Case
   * - ``BestEffort``
     - Fire and forget; may be silently dropped
     - Metrics, logs, non-critical events
   * - ``ObservableBestEffort``
     - Best-effort with delivery-result notification
     - Monitoring when drops should be visible
   * - ``AtLeastOnce``
     - Retry until acknowledged or deadline expires
     - Commands, state changes
   * - ``DurableAtLeastOnce``
     - At-least-once with durable outbox (design/backlog)
     - Financial transactions, critical workflows

.. code-block:: cpp

   DeliveryOptions opts;
   opts.mode = DeliveryMode::AtLeastOnce;
   opts.deadline = Clock::now() + std::chrono::seconds(5);
   opts.max_retries = 3;

   context()->send_with_options(target, message, opts);

Scheduled Delivery
------------------

Delay message delivery within an actor:

.. code-block:: cpp

   // Schedule self-delivery after 500ms
   auto handle = context()->schedule(
       std::chrono::milliseconds(500),
       ReminderMessage{...}
   );

   // Cancel a pending scheduled message
   context()->cancel_schedule(handle);

Scheduled messages use the scheduler's :cpp:class:`TimingWheel` for O(1)
insert and cancel.

Error Propagation
-----------------

Use ``context()->reply_with_error()`` to signal failures:

.. code-block:: cpp

   .on_request<DangerousOp, OpResult>([this](const DangerousOp& op) {
       if (!validate(op)) {
           context()->reply_with_error(error::invalid_argument);
           return;
       }
       // ... proceed with valid input
   });

The error code is encoded in an ``ErrorMsg`` envelope and delivered to
the sender's error handler.

Message Serialization
---------------------

For remote delivery, messages pass through :cpp:class:`DefaultSerializer`:

1. **Encode**: Protobuf message → wire bytes (4-byte BE TypeTag prefix +
   serialized protobuf payload).
2. **Frame**: Wrapped in a ``Frame`` with source/destination metadata and
   optional trace context.
3. **Transport**: Sent over TCP via :cpp:class:`TcpTransport`.

For local delivery, messages use in-memory protobuf objects — no
serialization overhead.

For more on delivery guarantees and failure handling, see
:doc:`/best-practices/error-handling`.
