Actor Lifecycle
================

HPActor Python actors have an explicit lifecycle with hooks for startup,
shutdown, failure, and restart.

Lifecycle Hooks
---------------

.. code-block:: python

   class MyActor(Actor):
       def on_start(self) -> None:
           print("Actor starting")

       def on_stop(self) -> None:
           print("Actor stopping")

       def on_restart(self) -> None:
           print("Actor restarted with new generation")

       def behavior(self) -> Behavior:
           return Behavior()

Supervision
-----------

By default, actors are supervised with at-most-once restart.  If a
handler raises an exception, the actor is restarted with a new
generation.  The new generation prevents stale dispatches and
completions from affecting the replacement incarnation.

Links and Monitors
------------------

- ``link_to(ref)`` — bidirectional death notification
- ``monitor(ref)`` — unidirectional death watching
- ``DownEvent`` — received when a linked or monitored actor exits

Generation Fencing
------------------

Every restart advances the actor's generation.  Dispatches and
completions from previous generations are silently discarded.  This
guarantees that a restarted actor never processes stale work.

Quarantine
----------

After repeated failures within a configurable window, an actor may be
quarantined instead of restarted.  Quarantined actors refuse all
messages.
