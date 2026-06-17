.. _getting-started-first-actor:

Your First Actor
================

This guide walks through building a minimal echo actor — the "Hello World"
of actor systems — in under 5 minutes.

Complete Example
----------------

.. code-block:: cpp

   #include <hpactor/core/actor_system.hpp>
   #include <hpactor/actor/event_based_actor.hpp>
   #include <hpactor/actor/actor_context.hpp>
   #include <iostream>

   using namespace hpactor;

   // 1. Define an echo actor
   class EchoActor : public EventBasedActor {
   protected:
       Behavior make_behavior() override {
           return Behavior::make()
               .on<StringMsg>([this](const StringMsg& msg) {
                   std::cout << "Received: " << msg.text << std::endl;
                   // Echo back to sender
                   context()->reply(msg);
               });
       }
   };

   // 2. Main — create the system, spawn, and send
   int main() {
       ActorSystem system;

       // Spawn the echo actor
       auto echo_ref = system.spawn<EchoActor>();

       // Use a ScopedActor to send and receive from a non-actor context
       ScopedActor client(system);
       client.send(echo_ref, StringMsg{"Hello, HPActor!"});

       // Receive the reply
       auto reply = client.receive<StringMsg>();
       std::cout << "Reply: " << reply->text << std::endl;

       return 0;
   }

Step-by-Step Breakdown
----------------------

1. **Include the headers.**
   ``actor_system.hpp`` provides the system, ``event_based_actor.hpp``
   provides the base class, and ``actor_context.hpp`` gives access to
   ``context()->send()`` and ``context()->reply()``.

2. **Define your actor.**
   Inherit from :cpp:class:`EventBasedActor` and override
   ``make_behavior()``. The behavior describes how the actor responds
   to each message type.

3. **Register message handlers.**
   ``Behavior::make()`` returns a builder. Call ``.on<MsgType>(handler)``
   for each message the actor handles. Each handler receives a reference
   to the message and runs on the actor's execution context.

4. **Create the ActorSystem.**
   The :cpp:class:`ActorSystem` is the runtime environment. It owns the
   scheduler, registry, and transport.

5. **Spawn.**
   ``system.spawn<EchoActor>()`` creates an instance and returns an
   :cpp:type:`ActorRef` — a handle that can be used to send messages.

6. **Send and receive from outside.**
   :cpp:class:`ScopedActor` is a convenience for non-actor threads to
   participate in message exchange.

Building and Running
--------------------

Save the example as ``echo.cpp`` and build:

.. code-block:: bash

   g++ -std=c++20 echo.cpp -I path/to/HPActor/include \
       -L path/to/HPActor/build -lhpactor -lprotobuf -lssl -lcrypto \
       -o echo

   ./echo

Expected output:

.. code-block:: text

   Received: Hello, HPActor!
   Reply: Hello, HPActor!

What's Next
-----------

- :doc:`/building-applications/actor-types` — understand when to use EventBased vs. Stateful vs.
  Typed vs. Blocking actors.
- :doc:`/building-applications/message-passing` — learn about protobuf-native
  handlers, request-response, and error replies.
- :doc:`/building-applications/lifecycle` — spawn trees, supervision, and
  graceful shutdown.
