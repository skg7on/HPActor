Installation
============

Requirements
------------

- **CPython >= 3.11** (3.11, 3.12, 3.13, 3.14 supported via ABI3)
- **protobuf >= 7.35.0, < 8**
- Linux: glibc 2.28 or newer (manylinux_2_28)
- macOS: 12.0 or newer

Windows, musllinux, PyPy, and free-threaded CPython are not supported in
this release.

Install
-------

.. code-block:: bash

   pip install hpactor

Verify
------

.. code-block:: python

   import hpactor
   print(hpactor.__version__)

The import must not start threads, open network connections, or register
file descriptors.  All runtime activity begins only when an
``ActorSystem`` is explicitly started.

Uninstall
---------

.. code-block:: bash

   pip uninstall hpactor

Source Build
------------

To build from source, you need CMake 3.26+, Ninja, a C++20 compiler, and
the checksum-locked native dependencies (OpenSSL 3.5.5, Abseil 20260107.1,
protobuf 35.0).  See ``bindings/python/packaging/README.md`` for the
hermetic build procedure.
