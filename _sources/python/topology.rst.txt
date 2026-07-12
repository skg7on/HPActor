Python Declarative Topology
===========================

HPActor supports declaring Python actors alongside C++ actors in the existing
``[[actors]]`` TOML topology.  A namespaced behavior reference tells the
bootstrap engine to import, validate, construct, and start a Python actor
class on the dedicated Python runtime thread.

.. warning::

   Python topology is **trusted executable configuration**.  Importing a
   module listed in the topology file executes its top-level code.
   Applications must keep the topology file and installed packages under
   the same deployment trust controls as native executable configuration.

   This is **not** a sandbox.

Quick Start
-----------

1. Define an actor class decorated with ``@actor("name")``:

   .. code-block:: python

       from hpactor import Actor, actor

       @actor("echo")
       class EchoActor(Actor):
           def __init__(self, prefix: str = ""):
               super().__init__()
               self._prefix = prefix

           async def on_start(self) -> None:
               pass

           def behavior(self):
               from hpactor import Behavior
               b = Behavior()
               b.on("StringValue", self._echo)
               return b

           async def _echo(self, msg, ctx):
               result = type(msg)()
               result.value = self._prefix + msg.value
               await ctx.reply(result)

2. Declare the actor in TOML:

   .. code-block:: toml

       [system.python]
       enabled = true
       topology_start_timeout_ms = 30000

       [[actors]]
       id = "echo"
       behavior = "python:my_app.actors:EchoActor"
       args = { prefix = "prod:" }

3. Supply an application-side module allowlist and load:

   .. code-block:: python

       from hpactor._system import ActorSystem
       from hpactor._topology import PythonTopologyPolicy
       from hpactor._messages import MessageRegistry

       registry = MessageRegistry()
       registry.freeze()

       system = ActorSystem.from_topology(
           "config/topology.toml",
           messages=registry,
           policy=PythonTopologyPolicy(
               allowed_module_prefixes=("my_app.actors",),
           ),
       )

       async with system:
           echo = system.resolve("echo")
           # ... send / ask ...

Behavior Reference Syntax
-------------------------

.. productionlist::
   behavior: "python:" module ":" qualname
   module: segment ("." segment)*
   qualname: segment ("." segment)*
   segment: [A-Za-z_] [A-Za-z0-9_]*

The ``module`` and every dotted segment must match ``[A-Za-z_][A-Za-z0-9_]*``.
``qualname`` uses the same grammar so module-level classes and nested classes
are representable.  Empty segments, relative imports, slashes, backslashes,
whitespace, ``<locals>``, NUL bytes, and more than one module/class separator
are rejected.

**Limits:**

+-------------------------------------+-------------------+
| Field                               | Maximum            |
+=====================================+===================+
| ``module``                          | 255 UTF-8 bytes    |
+-------------------------------------+-------------------+
| ``qualname``                        | 255 UTF-8 bytes    |
+-------------------------------------+-------------------+
| Complete behavior value             | 518 bytes          |
+-------------------------------------+-------------------+
| Actor argument key                  | 128 UTF-8 bytes    |
+-------------------------------------+-------------------+
| Actor argument value                | 4096 UTF-8 bytes   |
+-------------------------------------+-------------------+
| Arguments per actor                 | 128               |
+-------------------------------------+-------------------+
| Combined key/value bytes per actor  | 64 KiB            |
+-------------------------------------+-------------------+

Actor Arguments
---------------

TOML actor arguments are passed to the Python constructor as keyword
arguments:

.. code-block:: toml

    args = { prefix = "prod", retries = "3" }

.. code-block:: python

    class EchoActor(Actor):
        def __init__(self, prefix: str = "", retries: str = "1"):
            ...

The existing TOML parser canonicalizes integer, floating-point, and Boolean
scalar values into strings.  Arrays, nested inline tables, byte strings, and
positional arguments are rejected.

Keys must be valid Python identifiers, must not start with ``__hpactor_``,
and must be at most 128 UTF-8 bytes.  Actor argument values are never
emitted to errors, logs, metrics, CLI, or health snapshots — they may
contain secrets.

Constructor signature validation uses ``inspect.signature(actor_class).bind(**args)``
without constructing the actor.  A required positional-only parameter,
missing required keyword, or unexpected keyword fails before any actor is
published.

Module Allowlist
----------------

Declaring a Python actor in TOML requires an application-supplied
``PythonTopologyPolicy``.  The topology file **cannot** widen the allowlist.

.. code-block:: python

    from hpactor._topology import PythonTopologyPolicy

    policy = PythonTopologyPolicy(
        allowed_module_prefixes=("my_app.actors", "my_app.workers"),
    )

An allowed prefix matches the exact module **or** any child separated by
a dot:

- ``my_app.actors`` permits ``my_app.actors`` and ``my_app.actors.billing``
- ``my_app.actors`` does **not** permit ``my_app.actors_evil``

The policy must be non-empty when the topology contains any Python behavior.
Prefixes use the same absolute module grammar as behavior references.

The policy fingerprint (FVN-1a 64-bit over sorted, length-prefixed prefix
bytes) is included in the runtime blueprint fingerprint.  A policy change
requires a restart.

ActorSystem.from_topology()
----------------------------

.. py:classmethod:: ActorSystem.from_topology(path, *, messages, policy, config=None)

   Construct a system for declarative topology bootstrap.

   :param str path: Absolute path to the TOML topology file.
   :param MessageRegistry messages: Frozen protobuf message registry.
   :param PythonTopologyPolicy policy: Application module allowlist.
   :param dict config: Optional Python binding configuration overrides.

   ``from_topology()`` is **side-effect free** — it stores the path, policy,
   and manifest.  No threads, file descriptors, imports, or native state
   are created until ``__aenter__`` is called.

``__aenter__`` performs the full bootstrap in order:

1. Construct and start the native runtime.
2. Parse TOML natively and extract Python actor descriptors.
3. Preflight the factory manifest on the dedicated Python loop:
   import each referenced module once, resolve qualified class names,
   validate ``Actor`` subclasses, and bind constructor signatures.
4. Bind factory tokens to native prepared topology specs.
5. Start the Python runtime thread with the frozen manifest.
6. Execute the topology bootstrap transaction:
   spawn bridge actors, push ``TopologyInstall`` dispatches, and await
   readiness from the Python runtime.
7. On any failure, roll back all actors in reverse order.

After ``__aenter__`` returns, the system is in ``Running`` state and
committed actor names are available:

.. code-block:: python

    async with system:
        echo = system.resolve("echo")  # raises KeyError if not found

Transactional Commit and Rollback
---------------------------------

Topology startup is an **all-or-none transaction**:

1. No actor name is externally visible until **every** actor is ready.
2. For each Python actor, the native startup thread:
   a. Reserves a readiness slot (keyed by factory token).
   b. Spawns a ``PythonBridgeActor`` (the native proxy).
   c. Pushes a ``TopologyInstall`` dispatch to the Python runtime queue.
   d. Awaits the ``TopologyReady`` / ``TopologyFailed`` completion
      (with a per-actor deadline from ``topology_start_timeout_ms``).
3. The Python runtime thread handles each ``TopologyInstall`` dispatch:
   a. Constructs ``actor_class(**frozen_args)``.
   b. Freezes the behavior against the frozen message registry.
   c. Creates a runner and installs it in the dispatch coordinator.
   d. Calls ``await actor.on_start()``.
   e. Signals native readiness via ``complete_topology_actor()``.
4. If every actor is ready, the transaction commits.
5. If **any** actor fails — import, class resolution, constructor,
   ``on_start()``, capacity, timeout, or bridge spawn — the transaction
   rolls back **every** actor in reverse order:

   - Cancels in-progress ``on_start()`` calls.
   - Runs ``on_stop()`` for actors whose ``on_start()`` succeeded.
   - Removes runners and factory records.
   - Stops bridge actors and drains their mailboxes.

   The primary error is preserved; secondary rollback failures accumulate
   bounded error bits and structured logs.
6. After a failed startup, no actor names, directory entries, scheduler
   registrations, bridges, runners, factory records, threads, or file
   descriptors remain.

Errors
------

.. py:class:: TopologyPhase

   Phases of the topology lifecycle:

   =================== ===========================================
   Value                Description
   =================== ===========================================
   ``PARSE``            Invalid TOML, import, template, or DAG
   ``POLICY``           Module outside application allowlist
   ``IMPORT``           ``importlib`` import failure
   ``CLASS_RESOLUTION`` Missing or non-class attribute
   ``CLASS_VALIDATION`` Not an ``Actor`` or invalid decorator
   ``CONSTRUCTOR_BINDING`` Constructor signature mismatch
   ``NATIVE_PREPARE``   Python reference grammar or limit violation
   ``ACTOR_START``      Constructor, behavior, or ``on_start()`` error
   ``COMMIT``           Duplicate or failed name registration
   ``ROLLBACK``         Cleanup failure (secondary, non-fatal)
   =================== ===========================================

.. py:class:: TopologyError(HPActorError)

   Error during declarative topology loading.

   .. py:attribute:: phase
      :type: TopologyPhase

   .. py:attribute:: actor_id
      :type: str | None

   .. py:attribute:: behavior
      :type: str | None

   .. py:attribute:: error_code
      :type: int

   .. py:attribute:: detail
      :type: str
      Detail is bounded to 4096 UTF-8 bytes.  Import tracebacks are
      logged but not embedded in the public exception.

   .. py:attribute:: rollback_bits
      :type: int
      Stable bitmask of secondary rollback errors (0 if none).

Execution-Domain Rules
----------------------

- **TOML parsing** — native C++ only.  No ``tomllib`` or third-party
  TOML library in Python.
- **Python imports, class objects, factory records, actor instances,
  behaviors, ``on_start()``, ``on_stop()``** — dedicated Python
  asyncio thread only.
- **Scheduler workers** — execute only ``PythonBridgeActor`` and
  value-only HPActor operations.  Never acquire the GIL.
- **Cross-thread records** — strings, IDs, fingerprints, generations,
  factory tokens, status codes, and bounded details only.  No
  ``PyObject*`` or borrowed Python memory.
- **Bridge queues** — bounded lock-free MPSC rings.  Rejected enqueue
  fails the topology transaction.
- **Stale records** — every record carries native-system generation and
  actor generation.  Mismatches are discarded and counted.

Configuration
-------------

.. code-block:: toml

    [system.python]
    enabled = true
    topology_start_timeout_ms = 30000

``topology_start_timeout_ms`` (default 30,000, range 100–300,000) is a
per-actor deadline guard.  It is **not** a sleep-based progress check.

Topology configuration changes are ``RestartRequired``.  There is no
hot reload for module references, classes, or actor membership.

Security Model
--------------

Python topology is **trusted executable configuration**:

- Uses ``importlib.import_module()`` with absolute names only.
- Never accepts a filesystem path or URL.
- Never mutates ``sys.path``.
- Never calls package entry-point discovery.
- Never evaluates an expression.
- Resolves only validated attribute segments with ``getattr()``.
- Never logs argument values.

This is not a sandbox.  Importing an allowed module executes its top-level
code.  Applications must keep the topology and allowed package installation
under the same deployment trust controls as native executable configuration.

Known Limitations
-----------------

- **TOML only** — binary topology (``.hpactor`` compiled format) does not
  support Python behaviors.  Attempting to load a binary topology
  containing Python actors raises ``TopologyError``.
- **Startup only** — Python actors declared in TOML are created at system
  startup.  Dynamic children must use ``ActorSystem.spawn()`` or
  ``ActorContext.spawn()``.
- **Local process only** — Python actors run in-process.  Remote
  placement of Python actors is not supported.
- **No hot reload** — topology changes require a restart.  Live class
  replacement, module reloading, and actor-tree diffing are not supported.
- **No inferred TypeTags** — protobuf ``TypeTag`` values are not
  registered from topology.  Use an explicit ``MessageRegistry``.
- **Imported modules persist** — ``sys.modules`` is not cleaned after a
  failed startup.  Python provides no safe general module-unload contract.
  All binding-owned classes, factory records, actors, runners, bridges,
  and callbacks are released.
