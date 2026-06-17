.. _building-applications-topology-config:

Topology Configuration
======================

HPActor can bootstrap an entire actor tree from a declarative TOML
configuration file — no imperative spawn code required. This chapter
covers writing and deploying topology configs.

Pipeline Overview
-----------------

.. code-block:: text

   TOML file(s) ──► TomlParser ──► TopologyModel ──► BootstrapEngine
                                                         │
                                                    Spawned Actors

1. **Parse** — ``TomlParser::parse()`` reads the TOML, resolves imports
   (glob patterns), and applies template inheritance (deep merge).
2. **Validate** — Topological sort (Kahn's algorithm) ensures DAG-correct
   spawn order.
3. **Bootstrap** — ``BootstrapEngine`` creates dispatchers and spawns
   actors in dependency order.
4. **Notify** — ``SystemInitTag`` is broadcast after the full topology
   is live, gating external traffic.

Basic Topology
--------------

.. code-block:: toml

   # topology.toml

   [system]
   scheduler_threads = 8
   node_name = "prod-node-01"

   [system.metrics]
   enabled = true
   metrics_path = "/metrics"

   [system.log]
   level = "info"
   sinks = ["stderr", "rotating_file"]

   [[actors]]
   name = "worker-pool"
   behavior = "worker"
   count = 4
   args = { queue_capacity = 1024 }

   [[actors]]
   name = "coordinator"
   behavior = "coordinator"
   depends_on = ["worker-pool"]

   [[actors]]
   name = "http-gateway"
   behavior = "http_gateway"
   args = { listen_port = 8080 }

   [[dispatchers]]
   name = "cpu-dispatcher"
   type = "dense_compute"
   thread_count = 4

Actor Definition Fields
-----------------------

.. list-table::
   :header-rows: 1

   * - Field
     - Type
     - Description
   * - ``name``
     - string
     - Unique actor instance name (required)
   * - ``behavior``
     - string
     - Registered actor type name (required)
   * - ``count``
     - integer
     - Number of instances to spawn (default: 1)
   * - ``args``
     - table
     - Key-value pairs passed to ``configure_from_args()``
   * - ``depends_on``
     - array
     - Actor names that must spawn first
   * - ``dispatcher``
     - string
     - Dispatcher to assign this actor to
   * - ``mailbox_capacity``
     - integer
     - Bounded mailbox capacity (0 = unbounded)
   * - ``mailbox_overflow``
     - string
     - Overflow policy: ``block``, ``drop_head``, ``drop_tail``, ``dlq``
   * - ``drain_policy``
     - string
     - Shutdown drain: ``complete``, ``drop``, ``timeout``
   * - ``drain_timeout_ms``
     - integer
     - Drain deadline in milliseconds
   * - ``priority_level``
     - integer
     - Mailbox priority lane (0 = highest)

Registering Actors
------------------

Actors must be registered with the factory before topology bootstrapping:

.. code-block:: cpp

   #include <hpactor/config/actor_factory_registry.hpp>

   // Static registration — runs before main()
   HPACTOR_REGISTER_ACTOR(WorkerActor, "worker");
   HPACTOR_REGISTER_ACTOR(CoordinatorActor, "coordinator");
   HPACTOR_REGISTER_ACTOR(HttpGatewayActor, "http_gateway");

The ``HPACTOR_REGISTER_ACTOR`` macro:
- Maps the behavior name string to a factory function.
- Runs at static initialization time (no manual registry calls).
- Supports passing ``args`` to ``configure_from_args()``.

Template Inheritance
--------------------

Reduce repetition with a base template:

.. code-block:: toml

   # base.toml
   [template.default_worker]
   behavior = "worker"
   count = 4
   args = { queue_capacity = 1024 }
   mailbox_capacity = 10000
   mailbox_overflow = "dlq"

.. code-block:: toml

   # prod.toml
   [[imports]]
   path = "base.toml"

   [[actors]]
   name = "critical-worker-pool"
   inherit = "default_worker"
   count = 8  # overrides base count
   args = { queue_capacity = 4096 }  # deep-merged, not replaced

Imports
-------

Include other TOML files with glob support:

.. code-block:: toml

   [[imports]]
   path = "actors/*.toml"

   [[imports]]
   path = "environments/production.toml"

Bootstrap in Code
-----------------

.. code-block:: cpp

   #include <hpactor/core/actor_system.hpp>

   int main() {
       ActorSystem system;

       // Load and bootstrap the topology
       system.load_topology("config/topology.toml");

       // System is running. Block until shutdown.
       system.await_shutdown();
       return 0;
   }

AOT Compilation (Production)
----------------------------

For production deployments, pre-compile TOML to a compact binary format
with zero-parse mmap bootstrap:

.. code-block:: bash

   ./build/tools/toml-compiler/hpactor_toml_compiler \
       --input config/topology.toml \
       --output config/topology.bin

   # Then load the binary:
   system.load_topology("config/topology.bin");  // detects binary format automatically

Extending with Subsystem Parsers
---------------------------------

Add new TOML config sections without modifying the core parser. Create a
source file in ``src/config/parsers/``:

.. code-block:: cpp

   // src/config/parsers/my_subsystem_parser.cpp
   #include <hpactor/config/toml_parser_registry.hpp>
   #include <hpactor/config/toml_table_view.hpp>

   namespace {

   class MySubsystemParser : public TomlSystemParser {
       void parse(TomlTableView table, TopologyModel& model) override {
           // Parse your subsystem config from the table
       }
   };

   // Self-registers — no edits to parse_file_data needed
   TomlSystemParserRegistration<MySubsystemParser> registration;

   }  // anonymous namespace

For more on writing parsers, see the C++ API in
``include/hpactor/config/toml_parser_registry.hpp``.
