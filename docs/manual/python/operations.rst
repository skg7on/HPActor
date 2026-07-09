Operations
==========

The Python binding exposes bounded observability surfaces for runtime
inspection, health, and monitoring.

Health
------

The runtime is healthy when running with a fresh heartbeat.  Queue
pressure alone does not make the node unhealthy.

Inspection
----------

The ``/python status`` CLI command returns a bounded snapshot of all
Python actors including generation, handled count, failures, restarts,
and pending turns.

.. code-block:: text

   /python status

Dead Letter Queue
-----------------

Messages that cannot be delivered are recorded in the Dead Letter Queue
(DLQ).  CLI commands allow listing, inspecting, and replaying DLQ
entries:

.. code-block:: text

   /dlq list [actor_id]
   /dlq show <index>
   /dlq replay <index> [target]

Metrics
-------

The binding emits structured metrics for dispatch counts, handler
latency, failures, restarts, and shutdown events.

Graceful Shutdown
-----------------

``async with ActorSystem`` drains in-flight messages before stopping
the Python runtime thread.  After exit, no callbacks, Python
references, or notifier events remain.
