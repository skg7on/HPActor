.. _getting-started-project-structure:

Project Structure
=================

A well-organized HPActor project separates actor definitions, message
schemas, configuration, and application bootstrap code. This guide
recommends a layout that scales from small prototypes to large distributed
systems.

Recommended Layout
------------------

.. code-block:: text

   my-project/
   ├── CMakeLists.txt                # Top-level build
   ├── proto/                        # Protobuf message definitions
   │   ├── messages.proto
   │   └── commands.proto
   ├── src/
   │   ├── actors/                   # Actor implementations
   │   │   ├── worker_actor.hpp
   │   │   ├── worker_actor.cpp
   │   │   ├── coordinator_actor.hpp
   │   │   └── coordinator_actor.cpp
   │   ├── messages/                 # Message helpers, serialization
   │   │   └── message_utils.hpp
   │   └── main.cpp                  # Entry point: ActorSystem bootstrap
   ├── config/
   │   └── topology.toml             # Declarative actor tree topology
   ├── tests/
   │   ├── unit/
   │   │   └── test_worker_actor.cpp
   │   ├── integration/
   │   │   └── test_coordinator_flow.cpp
   │   └── system/
   │       └── test_end_to_end.cpp
   ├── deploy/
   │   └── systemd/
   │       └── my-project.service
   └── docs/
       └── architecture.md

CMakeLists.txt
--------------

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(MyProject LANGUAGES CXX)

   set(CMAKE_CXX_STANDARD 20)
   set(CMAKE_CXX_STANDARD_REQUIRED ON)

   # Include HPActor
   add_subdirectory(third_party/HPActor)

   # Protobuf codegen
   find_package(Protobuf REQUIRED)
   protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/messages.proto)

   # Build actor library
   add_library(my_actors STATIC
       src/actors/worker_actor.cpp
       src/actors/coordinator_actor.cpp
       ${PROTO_SRCS}
   )
   target_link_libraries(my_actors PUBLIC hpactor_lib protobuf::libprotobuf)
   target_include_directories(my_actors PUBLIC src ${CMAKE_CURRENT_BINARY_DIR})

   # Main executable
   add_executable(my_app src/main.cpp)
   target_link_libraries(my_app PRIVATE my_actors)

   # Tests
   enable_testing()
   add_subdirectory(tests)

Actor Header Conventions
------------------------

.. code-block:: cpp

   #pragma once

   #include <hpactor/actor/event_based_actor.hpp>
   #include <hpactor/actor/stateful_actor.hpp>
   // ... project headers

   namespace my_project {

   /// @brief Processes work items from a bounded queue.
   ///
   /// WorkerActor receives WorkRequest messages, processes them on the
   /// cooperative scheduler, and replies with WorkResponse.
   class WorkerActor : public EventBasedActor {
   protected:
       Behavior make_behavior() override;
       void on_exit() override;
   };

   }  // namespace my_project

Actor Registration
------------------

If using declarative TOML topology bootstrapping, register each actor
type with the factory:

.. code-block:: cpp

   // In worker_actor.cpp
   #include <hpactor/config/actor_factory_registry.hpp>

   HPACTOR_REGISTER_ACTOR(WorkerActor, "worker");

Using a topology config (``config/topology.toml``):

.. code-block:: toml

   [[actors]]
   name = "worker-pool-1"
   behavior = "worker"
   count = 4
   args = { queue_capacity = 1024 }

   [[actors]]
   name = "coordinator"
   behavior = "coordinator"

   [system]
   scheduler_threads = 8

Bootstrap in ``main.cpp``:

.. code-block:: cpp

   #include <hpactor/core/actor_system.hpp>

   int main(int argc, char* argv[]) {
       ActorSystem system;
       system.load_topology("config/topology.toml");
       // System is now running with the configured actor tree.
       // Block until shutdown:
       system.await_shutdown();
       return 0;
   }

Separation of Concerns
----------------------

.. list-table::
   :header-rows: 1

   * - Directory
     - Purpose
     - Constraints
   * - ``src/actors/``
     - Actor class implementations
     - One actor per .hpp/.cpp pair; no shared mutable state
   * - ``proto/``
     - Message schemas
     - Backward-compatible evolution; document TypeTag assignments
   * - ``config/``
     - TOML topology files
     - Per-environment; no secrets
   * - ``tests/``
     - Three-tier test structure
     - Deterministic; no timing assumptions
   * - ``deploy/``
     - Platform-specific deployment configs
     - systemd unit files, Dockerfiles, etc.

For more on testing conventions, see :doc:`/best-practices/testing`.
