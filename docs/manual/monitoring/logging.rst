.. _monitoring-logging:

Structured Logging
==================

HPActor includes a structured logging subsystem with a lock-free ring
buffer, pluggable sinks, and multiple output formats.

Architecture
------------

.. code-block:: text

   Actor/Component ──► LogRingBuffer ──► LogDrain ──► ILogSink
                     (lock-free MPSC)  (consumer)    ├── StderrSink
                                                     ├── FileSink
                                                     ├── RotatingFileSink
                                                     ├── SyslogSink
                                                     └── MemorySink

The pipeline:
1. Producers write log entries to the MPSC ring buffer (non-blocking).
2. A drain thread bathes entries and writes to configured sinks.
3. Sinks are pluggable — implement :cpp:class:`ILogSink` for custom destinations.

Log Levels
----------

.. list-table::
   :header-rows: 1

   * - Level
     - Description
     - When to Use
   * - ``trace``
     - Detailed diagnostic traces
     - Debugging message routing, scheduler decisions
   * - ``debug``
     - Developer debugging info
     - Actor state transitions, timing data
   * - ``info``
     - Normal operational events
     - Actor spawn/stop, configuration loaded
   * - ``warn``
     - Potentially problematic
     - Mailbox approaching capacity, retry attempts
   * - ``error``
     - Error conditions
     - Delivery failures, supervision restarts
   * - ``fatal``
     - Unrecoverable errors
     - Corrupt state, system shutdown

Log Categories
--------------

Categories enable filtering and routing:

.. code-block:: cpp

   LOG_INFO("actor.lifecycle", "Actor {} spawned", actor_id);
   LOG_WARN("mailbox.pressure", "Mailbox {} at {}% capacity",
            actor_id, pressure_pct);
   LOG_ERROR("delivery.failure", "Delivery to {} failed: {}",
             target, failure_reason);

Built-in categories:

- ``actor.lifecycle`` — spawn, stop, restart events.
- ``mailbox.pressure`` — capacity warnings, overflow decisions.
- ``delivery.failure`` — send errors, DLQ routing.
- ``scheduler.dispatch`` — dispatch decisions, work stealing.
- ``network.connection`` — connect, disconnect, TLS handshake.
- ``supervision.restart`` — child failure and restart decisions.
- ``config.reload`` — configuration changes (future).

Configuration
-------------

.. code-block:: toml

   [system.log]
   level = "info"                    # Minimum level to emit
   sinks = ["stderr", "rotating_file"]

   [system.log.rotating_file]
   path = "/var/log/hpactor/hpactor.log"
   max_size_mb = 100
   max_files = 10

   [system.log.syslog]
   enabled = false                   # Only for daemon mode
   facility = "local0"

Output Formats
--------------

Two built-in formatters:

**TextFormatter** (human-readable):

.. code-block:: text

   2026-06-17T10:15:30.123Z [info] actor.lifecycle Actor worker-1 spawned
   2026-06-17T10:15:30.456Z [warn] mailbox.pressure Mailbox worker-1 at 85% capacity

**JsonFormatter** (machine-parsable):

.. code-block:: json

   {
     "timestamp": "2026-06-17T10:15:30.123Z",
     "level": "info",
     "category": "actor.lifecycle",
     "message": "Actor worker-1 spawned",
     "trace_id": "0a1b2c3d4e5f6789abcdef0123456789",
     "span_id": "1234567890abcdef"
   }

For JSON logs, each entry includes the W3C trace context when tracing is
enabled — enabling log-to-trace correlation in Loki, ELK, or similar tools.

Compile-Time Disable
--------------------

.. code-block:: bash

   cmake -DENABLE_ACTOR_LOGGING=OFF ..

When disabled, all logging macros become no-ops with zero runtime overhead.

Sink Selection Guide
--------------------

.. list-table::
   :header-rows: 1

   * - Sink
     - Use Case
   * - ``StderrSink``
     - Development, debugging, container stdout
   * - ``FileSink``
     - Simple file output for small deployments
   * - ``RotatingFileSink``
     - Production file logging with size-based rotation
   * - ``SyslogSink``
     - Daemon mode, systemd-journald integration
   * - ``MemorySink``
     - Testing, inspection via CLI ``/log show``

For integration with log aggregation systems, see
:doc:`/sre-integration/logging-loki`.
