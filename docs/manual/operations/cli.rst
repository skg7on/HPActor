.. _operations-cli:

Interactive CLI
===============

HPActor includes an interactive command-line interface for real-time
introspection and administration of a running actor system.

Accessing the CLI
-----------------

The CLI is **opt-in at runtime** (disabled by default). Enable it via TOML:

.. code-block:: toml

   [system.cli]
   enabled = true
   listen_path = "/tmp/hpactor-cli.sock"   # Unix domain socket (default)
   # tcp_port = 9999                        # Or TCP
   default_format = "pretty"                # pretty, json, or tabular
   page_size = 20

When enabled, the :cpp:class:`CliActor` starts listening on stdin (if
running in the foreground) or on the configured socket.

.. code-block:: text

   $ hpactor-cli --connect /tmp/hpactor-cli.sock
   HPActor CLI v0.1.0
   Type /help for available commands.

   hpactor>

Command Architecture
--------------------

The CLI uses a **trie-based command tree**. Commands are organized
hierarchically with auto-completion, fuzzy suggestion (Levenshtein
distance ≤ 2), and paged output for long results.

.. code-block:: text

   /<subsystem> <action> [arguments...] [--flags]

Actor Commands
--------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``/actor list [filter]``
     - List all actors with status and type
   * - ``/actor <id> show``
     - Detailed state, mailbox depth, processing stats
   * - ``/actor <id> kill``
     - Terminate an actor (sends KillRequest)
   * - ``/actor <id> inspect``
     - Request InspectState from an actor
   * - ``/actor <id> trace``
     - Show recent trace spans for this actor

Example output:

.. code-block:: text

   hpactor> /actor list
   ┌─────────────┬──────────────┬──────────┬────────┬──────────┐
   │ ActorId     │ Type         │ State    │ Mailbox│ Latency  │
   ├─────────────┼──────────────┼──────────┼────────┼──────────┤
   │ actor:1     │ MetricsActor │ Running  │ 0      │ 1.2ms    │
   │ actor:2     │ CliActor     │ Running  │ 1      │ 0.8ms    │
   │ actor:3     │ Worker-1     │ Running  │ 45     │ 3.4ms    │
   │ actor:4     │ Worker-2     │ Running  │ 12     │ 2.1ms    │
   │ actor:5     │ Coordinator  │ Running  │ 3      │ 5.6ms    │
   └─────────────┴──────────────┴──────────┴────────┴──────────┘

System Commands
---------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``/system stats``
     - System-wide statistics (actors, messages, memory)
   * - ``/system drain``
     - Initiate graceful drain of all actors
   * - ``/system stop``
     - Finalize system shutdown
   * - ``/system config``
     - Show current system configuration

DLQ Commands
------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``/dlq list [actor_id]``
     - List dead-letter records, optionally filtered
   * - ``/dlq show <index>``
     - Show full details of a DLQ record
   * - ``/dlq replay <index> [target]``
     - Replay a DLQ record to original or specified target
   * - ``/dlq export [actor_id]``
     - Export DLQ records as JSON

Fault Injection Commands
------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``/fault status``
     - Show fault injection controller state
   * - ``/fault list``
     - List active fault points and schedules
   * - ``/fault clear``
     - Clear all active fault injections

Ask Commands
------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``/ask pending``
     - List in-flight ask requests
   * - ``/ask cancel <msg_id>``
     - Cancel a pending ask request
   * - ``/ask stats``
     - Ask subsystem statistics

Failure Commands
----------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``/failure reasons``
     - List all FailureReason codes and descriptions
   * - ``/failure summary``
     - Recent failure statistics

Output Formats
--------------

Switch format with the ``--format`` flag:

- ``pretty`` — ANSI box-drawing tables (default, interactive).
- ``json`` — Machine-readable JSON output.
- ``tabular`` — Grep-friendly plain text.

.. code-block:: text

   /actor list --format json
   /dlq export --format json > dlq_dump.json

Pager
-----

For long output, the CLI enters paging mode:

.. code-block:: text

   -- More (20/150) [n=next, p=prev, q=quit, /search, g=go to line] --

Writing Custom Commands
-----------------------

Add commands by implementing :cpp:class:`ICommandHandler` and registering
with the command tree:

.. code-block:: cpp

   #include <hpactor/cli/command_node.hpp>

   class MyCustomCommand : public ICommandHandler {
       std::string name() const override { return "my-cmd"; }
       std::string help() const override { return "My custom command"; }

       void execute(const CommandContext& ctx) override {
           ctx.output() << "Hello from custom command!";
       }
   };

   // Register in your actor's initialization:
   CommandNode::root()->add_child("my-cmd",
       std::make_unique<MyCustomCommand>());
