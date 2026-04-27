// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// =============================================================================
// HPActor Example 05: Ping-Pong Actor Communication
// =============================================================================
//
// This example demonstrates actor-to-actor communication patterns including:
//   - hpactor::scoped_actor for non-actor contexts (main function)
//   - Actor-to-actor messaging via ActorContext::send()
//   - hpactor::ActorContext::reply() for response messages
//   - Actor linking with link_to() for death sharing
//   - Actor monitoring with demonitor()
//
// NOTE: This example demonstrates the intended API design. The runtime
// infrastructure (spawn, send, reply, scheduler loop) is not yet functional.
//
// =============================================================================

#include <hpactor/actor/blocking_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor/scoped_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <iostream>
#include <variant>

// -----------------------------------------------------------------------------
// Ping-Pong Concepts
// -----------------------------------------------------------------------------
//
// The ping-pong pattern demonstrates the fundamental request-response pattern
// in actor systems:
//
//   Main/ScopedActor
//        |
//        | send(pong_addr, PingMessage{count})
//        v
//      PongActor
//        |
//        | reply(PongMessage{count})
//        v
//   (implicit sender, which is PingActor in a full system)
//
// With linking, actors share failure:
//
//   PingActor --link_to--> PongActor
//        |
//        | If PongActor dies:
//        v
//   PingActor receives down_msg and can react
//
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Message Definitions
// -----------------------------------------------------------------------------

struct PingMessage {
    int count; // Number of remaining pings
};

struct PongMessage {
    int count; // Number of remaining pongs
};

struct StartMessage {
    int initial_count;
};

struct StopMessage {};

// -----------------------------------------------------------------------------
// PingActor - sends pings and handles pongs
// -----------------------------------------------------------------------------

class PingActor : public hpactor::EventBasedActor {
  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[](hpactor::TypedMessage& /*msg*/) {
            // In a real implementation, would use std::visit to handle messages
            std::cout << "PingActor: received message" << std::endl;
        }};
    }

  public:
    PingActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}
};

// -----------------------------------------------------------------------------
// PongActor - receives pings and sends pongs
// -----------------------------------------------------------------------------

class PongActor : public hpactor::EventBasedActor {
  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[](hpactor::TypedMessage& /*msg*/) {
            std::cout << "PongActor: received message" << std::endl;
        }};
    }

  public:
    PongActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}
};

// -----------------------------------------------------------------------------
// ScopedActor Pattern
// -----------------------------------------------------------------------------
//
// hpactor::ScopedActor is a blocking actor for non-actor contexts like main().
// It allows the main thread to participate in the actor system.
//
// Pattern:
//   hpactor::ScopedActor scope(system);
//
//   // Spawn actors
//   auto ping = system.spawn<PingActor>();
//   auto pong = system.spawn<PongActor>();
//
//   // Set pong address in ping actor
//   scope.send(ping.address(), SetPongMessage{pong.address()});
//
//   // Start the exchange
//   scope.send(ping.address(), StartMessage{5});
//
//   // Block until done
//   scope.receive<StopMessage>();
//
// -----------------------------------------------------------------------------

void demonstrate_scoped_actor() {
    std::cout << "=== ScopedActor Pattern ===" << std::endl;

    std::cout << R"(
ScopedActor allows non-actor code (like main()) to:
  - Send messages to actors
  - Receive responses (blocking)
  - Link to actors for death notification
  - Act as an actor itself

Pattern:
  hpactor::ScopedActor scope(system);

  // Spawn actors
  auto actor = system.spawn<MyActor>();

  // Send messages
  scope.send(actor.address(), MyMessage{});

  // Receive with timeout
  auto msg = scope.receive<MyResponse>(std::chrono::seconds(5));
)" << std::endl;
}

// -----------------------------------------------------------------------------
// Linking Pattern
// -----------------------------------------------------------------------------
//
// Linking creates a death-sharing relationship between actors:
//
//   ActorA --link_to--> ActorB
//     |
//     +-- If ActorB dies, ActorA receives down_msg
//     +-- If ActorA dies, ActorB receives down_msg
//
// This is useful for:
//   - Actor groups that should fail together
//   - Cleanup when dependencies die
//   - Cascading shutdown
//
// -----------------------------------------------------------------------------

void demonstrate_linking() {
    std::cout << "\n=== Linking Pattern ===" << std::endl;

    std::cout << R"(
Linking creates death-sharing relationships:

  actor->link_to(other_addr);

  // If other_addr dies, actor receives:
  context()->receive(down_msg{other_addr, reason});

  // Unlink to remove the relationship:
  actor->unlink_from(other_addr);

Use cases:
  - Worker processes linked to their supervisor
  - Resource cleanup when dependency dies
  - Cascading shutdown propagation
)" << std::endl;
}

// -----------------------------------------------------------------------------
// Monitoring Pattern
// -----------------------------------------------------------------------------
//
// Monitoring is one-way death notification (unlike linking):
//
//   ActorA --monitors--> ActorB
//     |
//     +-- If ActorB dies, ActorA receives down_msg
//     +-- But if ActorA dies, ActorB is unaffected
//
// Unlike linking, monitoring doesn't affect the monitored actor.
//
// -----------------------------------------------------------------------------

void demonstrate_monitoring() {
    std::cout << "\n=== Monitoring Pattern ===" << std::endl;

    std::cout << R"(
Monitoring is one-way death notification:

  context()->monitor(target_addr);

  // When target dies, this actor receives:
  down_msg{terminated_actor: target_addr, reason: error{}}

  // Stop monitoring:
  demonitor(target_addr);

Use cases:
  - Supervisors monitoring children (one-way)
  - Watchdogs that react to but don't affect others
  - Distributed system health monitoring
)" << std::endl;
}

// -----------------------------------------------------------------------------
// Main - demonstrates actor communication patterns
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== HPActor Example 05: Ping-Pong Actor Communication ==="
              << std::endl;

    demonstrate_scoped_actor();
    demonstrate_linking();
    demonstrate_monitoring();

    hpactor::Config config{.scheduler_threads = 4, .max_queue_depth = 1024};
    hpactor::ActorSystem system(config);

    std::cout << "\nNOTE: Actor spawning and message passing are not yet "
                 "fully implemented.\n"
              << "This example demonstrates the intended API usage patterns.\n"
              << std::endl;

    // -----------------------------------------------------------------
    // Pattern: Complete ping-pong exchange
    // -----------------------------------------------------------------
    // In a complete system:
    //
    //   hpactor::ActorSystem system(config);
    //   hpactor::ScopedActor scope(system);
    //
    //   // Spawn actors
    //   auto ping = system.spawn<PingActor>();
    //   auto pong = system.spawn<PongActor>();
    //
    //   // Set pong address in ping actor
    //   ping->set_pong_address(pong.address());
    //
    //   // Link ping to pong (if pong dies, ping reacts)
    //   ping->link_to(pong.address());
    //
    //   // Start the exchange
    //   ping->start(5);
    //
    //   // Or with typed actors:
    //   hpactor::typed_actor<
    //       hpactor::result<void>(SetPongMessage),
    //       hpactor::result<void>(StartMessage)
    //   > typed_ping = ping;
    //   typed_ping(SetPongMessage{pong.address()});
    //   typed_ping(StartMessage{5});

    std::cout << "\nKey communication APIs:" << std::endl;
    std::cout << "  context()->send(addr, message)  // Send to actor" << std::endl;
    std::cout << "  context()->reply(message)       // Reply to sender" << std::endl;
    std::cout << "  actor->link_to(addr)            // Death sharing" << std::endl;
    std::cout << "  context()->monitor(addr)         // One-way watch" << std::endl;
    std::cout << "  scope.receive<T>()               // Blocking receive"
              << std::endl;

    return 0;
}
