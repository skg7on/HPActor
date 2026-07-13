Python Binding
==============

HPActor ships an official Python binding with two complementary surfaces:

1. **In-process actor API** — an asyncio-first actor runtime backed by a
   pybind11-based ``_hpactor`` native extension.  Define actors, send
   messages, ask for replies, schedule timers, and participate in
   supervision trees — all from Python, interleaved with C++ actors.

2. **External client SDK** (``hpactor.client``) — a pure-Python package
   for consuming HPActor health, metrics, HTTP gateway, and CLI surfaces
   *without* importing the native runtime.  Sync and async clients share
   the same validated configuration and typed error model.

.. toctree::
   :maxdepth: 2
   :caption: Python Binding

   installation
   your-first-actor
   actor-api
   topology
   external-sdk
