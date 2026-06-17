.. _best-practices-error-handling:

Error Handling
==============

HPActor provides structured error handling through failure envelopes,
delivery modes, and dead-letter queues. This chapter covers patterns for
building resilient error handling into your actors.

Failure Envelope
----------------

When message delivery fails, HPActor generates a :cpp:class:`FailureEnvelope`
with full context:

.. code-block:: cpp

   struct FailureEnvelope {
       ActorId actor_id;         // The actor that experienced the failure
       ActorId sender;           // Original message sender (for notification)
       ActorId receiver;         // Intended recipient
       MessageId message_id;     // Correlated message
       TraceContext trace;       // W3C trace context
       FailureReason reason;     // Why it failed
       FailureSource source;     // Which subsystem (mailbox, transport, etc.)
       bool retryable;           // Can the operation be retried?
       Clock::time_point timestamp;
       std::string detail;       // Human-readable detail
   };

FailureReason Taxonomy
----------------------

23 failure reasons in 10 semantic ranges:

.. list-table::
   :header-rows: 1

   * - Range
     - Examples
     - Retryable?
   * - Delivery (1-9)
     - ``Timeout``, ``NoRoute``, ``Dropped``
     - Sometimes
   * - Mailbox (10-19)
     - ``MailboxFull``, ``Overflow``, ``Rejected``
     - Sometimes
   * - Lifecycle (20-29)
     - ``ActorNotRunning``, ``Quarantined``, ``CircuitOpen``
     - Usually no
   * - Resource (30-39)
     - ``OutOfMemory``, ``TooManyActors``
     - Usually no
   * - Serialization (40-49)
     - ``EncodeError``, ``DecodeError``
     - No
   * - Network (50-59)
     - ``ConnectionLost``, ``Timeout``
     - Sometimes
   * - Validation (60-69)
     - ``InvalidMessage``, ``TypeMismatch``
     - No
   * - Supervision (70-79)
     - ``RestartLimitExceeded``, ``Escalated``
     - No
   * - Shutdown (80-89)
     - ``SystemShuttingDown``, ``DrainTimeout``
     - No
   * - Unknown (90-99)
     - ``Unknown``
     - No

Delivery Modes: Choosing the Right Guarantee
--------------------------------------------

.. code-block:: cpp

   // Best-effort: fire and forget — no delivery confirmation
   context()->send(target, metrics_event);

   // Observable best-effort: same, but get a notification if it fails
   auto result = context()->send_observable(target, important_event);
   if (!result.ok()) {
       LOG_WARN("delivery.failure", "Failed to deliver: {}",
                result.failure_reason());
   }

   // At-least-once: retry with deadline
   DeliveryOptions opts;
   opts.mode = DeliveryMode::AtLeastOnce;
   opts.deadline = Clock::now() + std::chrono::seconds(5);
   opts.max_retries = 3;
   auto result = context()->send_with_options(target, critical_cmd, opts);

   // Durable at-least-once: survives actor restart (design/backlog)

Handling Errors in Actors
-------------------------

**Validate early, reply with errors:**

.. code-block:: cpp

   .on_request<Transfer, TransferResult>([this](const Transfer& req) {
       if (req.amount() <= 0) {
           context()->reply_with_error(error::invalid_argument);
           return;
       }
       if (req.from() == req.to()) {
           context()->reply_with_error(error::invalid_argument);
           return;
       }
       // ... proceed with valid transfer
   });

**Receive failure notifications:**

.. code-block:: cpp

   .on<FailureEnvelope>([this](const FailureEnvelope& failure) {
       LOG_ERROR("delivery.failure", "Delivery to {} failed: {}",
                 failure.receiver, failure.reason);
       if (failure.retryable) {
           // Retry logic — but don't retry forever
       }
   });

**Handle downstream timeouts:**

.. code-block:: cpp

   auto handle = context()->ask<Response>(target, request,
       RequestTimeout::after(std::chrono::seconds(3)));

   // Later, check for timeout:
   if (!handle.ready()) {
       // Handle timeout — fallback, retry, or fail
   }

Dead-Letter Queue (DLQ)
-----------------------

Messages that cannot be delivered are routed to the DLQ. The DLQ
retains full message payload, trace context, and failure reason.

**DLQ as a safety valve:**

.. code-block:: toml

   [[actors]]
   name = "critical-worker"
   behavior = "worker"
   mailbox_capacity = 10000
   mailbox_overflow = "dlq"     # Overflow → DLQ instead of dropping

**Inspecting the DLQ:**

.. code-block:: text

   /dlq list
   /dlq show 0
   /dlq replay 0                 # Retry delivery
   /dlq export --format json     # Export for offline analysis

**Programmatic DLQ access:**

.. code-block:: cpp

   auto& dlq = system.dead_letter_queue();
   auto snapshot = dlq.snapshot_records();
   for (const auto& record : snapshot) {
       if (record.reason == DeadLetterReason::MailboxFull) {
           // Investigate pressure
       }
   }

Circuit Breaker
---------------

For actors that call external services, use the circuit breaker to
prevent cascading failures:

.. code-block:: toml

   [system.quarantine]
   enabled = true

   [[actors]]
   name = "external-api-caller"
   behavior = "api_client"
   quarantine.enabled = true
   quarantine.trip_threshold = 5        # 5 consecutive failures → trip
   quarantine.cooldown_ms = 30000       # 30s cooldown before probe
   quarantine.half_open_max_probes = 3  # Allow 3 probes in half-open state

States:
- **Closed** — normal operation.
- **Open** — all requests rejected immediately (fast-fail).
- **Half-Open** — limited probes allowed to test recovery.

Error Handling Checklist
------------------------

- [ ] Every ``on_request`` handler validates input and returns errors for
  invalid input.
- [ ] Critical messages use ``AtLeastOnce`` delivery mode.
- [ ] Mailboxes have bounded capacity with explicit overflow policies.
- [ ] DLQ is monitored and periodically drained.
- [ ] External service calls are protected by circuit breakers.
- [ ] Supervision policies limit restart frequency.
- [ ] Failure callbacks handle ``FailureEnvelope`` (log, retry, or escalate).
