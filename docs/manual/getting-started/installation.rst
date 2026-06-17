.. _getting-started-installation:

Installation
============

Prerequisites
-------------

HPActor requires:

- **C++20 compiler**: GCC 11+ or Clang 14+.
- **CMake 3.20+**.
- **Ninja** (recommended) or Make.
- **System libraries**: OpenSSL (dev), Protobuf (dev, with protoc).

On macOS:

.. code-block:: bash

   brew install cmake ninja openssl protobuf

On Ubuntu/Debian:

.. code-block:: bash

   sudo apt install cmake ninja-build libssl-dev protobuf-compiler \
                    libprotobuf-dev

On RHEL/Fedora:

.. code-block:: bash

   sudo dnf install cmake ninja-build openssl-devel protobuf-compiler \
                    protobuf-devel

Cloning the Repository
----------------------

.. code-block:: bash

   git clone https://github.com/skg7on/HPActor.git
   cd HPActor

Building HPActor
----------------

The standard build uses Ninja for fast incremental compilation:

.. code-block:: bash

   # Configure
   cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

   # Build the library and all targets
   ninja -C build

This produces:

- ``build/libhpactor.a`` — the HPActor runtime library.
- Test binaries under ``build/tests/``.
- Example binaries under ``build/examples/`` (if ``ENABLE_EXAMPLES=ON``).
- App binaries under ``build/apps/`` (if ``ENABLE_APPS=ON``).

Build Options
~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Option
     - Default
     - Description
   * - ``ENABLE_EXAMPLES``
     - ON
     - Build simple API usage examples
   * - ``ENABLE_APPS``
     - ON
     - Build complex demo applications
   * - ``ENABLE_PROACTOR``
     - OFF
     - Enable proactor I/O backend
   * - ``ENABLE_MEMORY_TRACKING``
     - ON
     - Per-actor memory allocation tracking
   * - ``ENABLE_MEMORY_DEBUG``
     - OFF
     - Memory poisoning + canary verification
   * - ``ENABLE_ACTOR_METRICS``
     - ON
     - Actor-level Prometheus-compatible metrics
   * - ``ENABLE_ACTOR_LOGGING``
     - ON
     - Structured actor logging subsystem
   * - ``ENABLE_ACTOR_TRACING``
     - ON
     - W3C-compatible distributed tracing
   * - ``ENABLE_CLI``
     - ON
     - Interactive CLI subsystem (runtime opt-in)
   * - ``ENABLE_COVERAGE``
     - OFF
     - gcov/llvm-cov coverage instrumentation
   * - ``ENABLE_CLANG_TIDY``
     - OFF
     - Run clang-tidy during C++ builds
   * - ``ENABLE_FAULT_INJECTION``
     - ON
     - Deterministic fault injection hooks

Example: minimal build for production:

.. code-block:: bash

   cmake -S . -B build -GNinja \
       -DENABLE_EXAMPLES=OFF \
       -DENABLE_APPS=OFF \
       -DENABLE_COVERAGE=OFF

Running Tests
-------------

.. code-block:: bash

   # Run the full test suite (parallel)
   ctest --output-on-failure --parallel 8

   # Run a single test binary
   ./build/tests/unit/core/test_unit_core

   # Run a specific test case
   ./build/tests/unit/core/test_unit_core --gtest_filter="*ActorId*"

   # Run via ctest pattern matching
   ctest -R "ActorIdDefaultConstruction" --output-on-failure

Sanitizer Builds
~~~~~~~~~~~~~~~~

.. code-block:: bash

   # ThreadSanitizer
   cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON
   ninja -C build-tsan

   # AddressSanitizer
   cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON
   ninja -C build-asan

Integrating with Your Project
-----------------------------

Add HPActor as a CMake subdirectory (recommended):

.. code-block:: cmake

   # In your project's CMakeLists.txt
   add_subdirectory(path/to/HPActor)

   add_executable(my_app main.cpp)
   target_link_libraries(my_app PRIVATE hpactor_lib)

Or link against an installed library:

.. code-block:: cmake

   find_package(hpactor REQUIRED)
   target_link_libraries(my_app PRIVATE hpactor::hpactor_lib)
