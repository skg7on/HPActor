Deployment
==========

Resource Sizing
---------------

- **Queue capacity**: powers of two (``1024``, ``2048``, etc.)
- **Drain budget**: ``32``--``256`` dispatches per turn
- **Shutdown timeout**: ``30s`` recommended

Each Python actor occupies one dispatch queue slot while a turn is
active.  Size queues to handle peak concurrent turns plus headroom.

Process Scaling
---------------

Run one HPActor process per CPU core rather than using subinterpreters.
Actors within a process are cooperatively scheduled on the dedicated
asyncio event loop.

Container Health Probes
-----------------------

The health endpoint returns HTTP 200 when the runtime is healthy:

.. code-block:: bash

   curl http://localhost:8080/health

Supported Platforms
-------------------

| Platform | Supported |
|----------|-----------|
| Linux manylinux_2_28 x86_64 | Yes |
| Linux manylinux_2_28 aarch64 | Yes |
| macOS 12.0 x86_64 | Yes |
| macOS 12.0 arm64 | Yes |
| Windows | No |
| musllinux | No |
| PyPy | No |
| free-threaded CPython | No |

Remote Nodes
------------

Native remote-node participation (cluster membership, remote spawn,
distributed actors) is deferred until identity, authorization, and
protocol negotiation exist.
