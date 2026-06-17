.. _operations-cli-server:

CLI Server & Remote Access
===========================

The :cpp:class:`CliServerActor` provides remote CLI access over Unix
domain sockets or TCP, enabling administration of production deployments.

Architecture
------------

.. code-block:: text

   ┌─────────────┐   UDS/TCP    ┌───────────────┐
   │ hpactor-cli │◄────────────►│ CliServerActor │──► ActorSystem
   └─────────────┘              └───────────────┘

The :cpp:class:`CliServerActor` decouples CLI transport from command
processing:

- **Transport** — UDS or TCP socket, managed by the server actor.
- **Session** — :cpp:class:`CliSession` is transport-agnostic; each
  connection gets its own session.
- **Authentication** — (future) TLS + mTLS for remote TCP connections.

Configuration
-------------

.. code-block:: toml

   [system.cli]
   enabled = true

   [system.cli.server]
   listen_path = "/tmp/hpactor-cli.sock"   # Unix domain socket
   # listen_addr = "0.0.0.0:9999"           # TCP (alternative)
   max_connections = 10

For daemon-mode deployments, the CLI server is the primary interaction
mechanism:

.. code-block:: toml

   [system.process]
   mode = "daemon"

   [system.cli]
   enabled = true

   [system.cli.server]
   listen_path = "/var/run/hpactor/cli.sock"

Standalone CLI Client
---------------------

The ``hpactor-cli`` binary connects to a running system:

.. code-block:: bash

   # Connect via Unix domain socket (default)
   hpactor-cli --connect /tmp/hpactor-cli.sock

   # Connect via TCP
   hpactor-cli --connect 192.168.1.10:9999

   # Execute a single command and exit
   hpactor-cli --connect /tmp/hpactor-cli.sock --command "/system stats"

   # Pipe commands from stdin
   echo "/actor list --format json" | hpactor-cli --connect /tmp/hpactor-cli.sock

Building the CLI Client
-----------------------

.. code-block:: bash

   ninja -C build hpactor-cli
   ./build/tools/hpactor-cli/hpactor-cli

The CLI client links only against the messaging and serialization
libraries — it does not embed the actor runtime.

Session Model
-------------

Each connection creates a :cpp:class:`CliSession`:

- Commands execute within the session's actor context.
- Sessions are isolated — one session's state does not leak to another.
- Sessions time out after a configurable idle period (default: 5 minutes).
- On disconnect, any in-progress commands are cancelled.

Security Considerations
-----------------------

- **UDS** — file permissions restrict access (default: ``0600``).
- **TCP** — no authentication currently; use only on trusted networks
  or behind a reverse proxy with mTLS.
- **(Future)** TLS + mTLS for authenticated remote access.

Programmatic Access
-------------------

For non-CLI tooling, prefer the HTTP API (:doc:`http-gateway`) or
direct ActorSystem API calls over CLI scripting.

.. code-block:: bash

   # Quick status check via CLI (lightweight)
   hpactor-cli --connect /tmp/hpactor-cli.sock --command "/system stats --format json"
