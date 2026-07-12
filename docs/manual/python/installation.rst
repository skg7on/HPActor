Installation
============

The ``hpactor`` package is distributed as ABI3 wheels for CPython 3.11+
and a universal ``py3-none-any`` client-only wheel.

Supported Platforms
-------------------

+------------------+---------+-----------------------------+
| Platform         | Arch    | Support                     |
+==================+=========+=============================+
| Linux (glibc)    | x86_64  | Native + Client SDK         |
+------------------+---------+-----------------------------+
| Linux (glibc)    | ARM64   | Native + Client SDK         |
+------------------+---------+-----------------------------+
| macOS 12.0+      | x86_64  | Native + Client SDK         |
+------------------+---------+-----------------------------+
| macOS 12.0+      | ARM64   | Native + Client SDK         |
+------------------+---------+-----------------------------+
| Any Python 3.11+ | any     | Client SDK only (no native) |
+------------------+---------+-----------------------------+

Install from PyPI
-----------------

.. code-block:: bash

    pip install hpactor

The correct wheel is selected automatically: native ``cp311-abi3`` on
supported Linux/macOS hosts, ``py3-none-any`` everywhere else.

Build from Source
-----------------

.. code-block:: bash

    # Install runtime Python dependencies
    pip install protobuf httpx

    # Clone and build
    git clone https://github.com/skg7on/HPActor.git
    cd HPActor
    cmake -S . -B build -GNinja -DENABLE_PYTHON_BINDINGS=ON
    ninja -C build _hpactor

    # Run the C++-side Python binding tests
    ctest --test-dir build -R PythonBinding --output-on-failure

    # Run the pure-Python tests
    PYTHONPATH=bindings/python:$PYTHONPATH \\
        python3 -m unittest discover -s bindings/python/tests/unit -v

    # Use the package from the build tree
    PYTHONPATH=bindings/python:$PYTHONPATH python3

Dependencies
------------

+-------------------+------------------+---------------------------+
| Package           | Version          | Required by               |
+===================+==================+===========================+
| ``protobuf``      | >=7.35.0,<8      | Native + Client SDK       |
+-------------------+------------------+---------------------------+
| ``httpx``         | >=0.28.1,<0.29   | Client SDK                |
+-------------------+------------------+---------------------------+

.. note::

   ``httpx`` is only required for the external client SDK.  The in-process
   actor API works with just ``protobuf``.

Verifying the Installation
--------------------------

.. code-block:: bash

    # Verify the native runtime is available
    python3 -c "from hpactor import ActorSystem; print('OK')"

    # Verify the client SDK works without the native runtime
    python3 -c "
    import hpactor.client
    import sys
    assert 'hpactor._hpactor' not in sys.modules
    print('OK: client SDK works without native runtime')
    "
