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
// HPActor Example 01: Echo Actor
// =============================================================================
//
// This example demonstrates the foundational pattern for creating an
// event-based actor in HPActor.
//
// Key concepts demonstrated:
//   - Subclassing hpactor::EventBasedActor
//   - Implementing make_behavior() to define initial message handling
//   - Using become() to dynamically switch behaviors at runtime
//   - Handling messages via std::visit on TypedMessage
//
// NOTE: This example demonstrates the intended API design. The runtime
// infrastructure (spawn, send, scheduler) is not yet functional.
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <iostream>
#include <string>
#include <variant>

// -----------------------------------------------------------------------------
// Message Definitions
// -----------------------------------------------------------------------------

struct EchoMessage {
    std::string text;
};

struct UppercaseMessage {
    std::string text;
};

struct ShutdownMessage {};

// -----------------------------------------------------------------------------
// EchoActor - demonstrates behavior-based message handling
// -----------------------------------------------------------------------------
//
// The EventBasedActor is the core building block for message-driven actors.
// It uses a Behavior object to handle messages and supports dynamic behavior
// switching via become().
//
// Pattern:
//   class MyActor : public hpactor::EventBasedActor {
//     protected:
//       hpactor::Behavior make_behavior() override {
//         return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
//           // Handle messages
//         }};
//       }
//     public:
//       MyActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
//         : hpactor::EventBasedActor(ctx, sys) {}
//   };
//
// NOTE: The actual TypedMessage in hpactor only contains system messages.
// User-defined messages would be added via a custom variant type.
//
// -----------------------------------------------------------------------------

class EchoActor : public hpactor::EventBasedActor {
  protected:
    // make_behavior() is called once when the actor activates
    // Override this to define your initial behavior
    hpactor::Behavior make_behavior() override {
        // Return a Behavior with a lambda that handles TypedMessage
        // In practice, you'd use std::visit to pattern match on message type
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            this->handle_message(msg);
        }};
    }

  public:
    EchoActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}

    // Message handler - in a real implementation, this would use std::visit
    void handle_message(hpactor::TypedMessage& /*msg*/) {
        // Pattern matching would look like:
        // std::visit([this](auto&& m) {
        //   using T = std::decay_t<decltype(m)>;
        //   if constexpr (std::is_same_v<T, EchoMessage>) { ... }
        // }, msg);
        std::cout << "EchoActor: received message" << std::endl;
    }
};

// -----------------------------------------------------------------------------
// SwitchingActor - demonstrates dynamic behavior switching with become()
// -----------------------------------------------------------------------------
//
// become() allows actors to change their behavior at runtime. This is the
// foundation of many actor patterns:
//
//   - State machines
//   - Protocol switching
//   - Dynamic message handling
//
// Pattern:
//   void MyActor::switch_to_new_behavior() {
//     become(hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
//       // New behavior handler
//     }});
//   }
//
// -----------------------------------------------------------------------------

class SwitchingActor : public hpactor::EventBasedActor {
  protected:
    hpactor::Behavior make_behavior() override {
        // Initial behavior - echo mode
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            this->handle_message_echo_mode(std::move(msg));
        }};
    }

  public:
    SwitchingActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}

    // Switch to uppercase mode
    void switch_to_uppercase_mode() {
        become(hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            this->handle_message_uppercase_mode(std::move(msg));
        }});
        std::cout << "SwitchingActor: switched to UPPERCASE mode" << std::endl;
    }

    // Switch back to echo mode
    void switch_to_echo_mode() {
        become(hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            this->handle_message_echo_mode(std::move(msg));
        }});
        std::cout << "SwitchingActor: switched to ECHO mode" << std::endl;
    }

  private:
    enum class Mode { Echo, Uppercase };

    void handle_message_echo_mode(hpactor::TypedMessage&& /*msg*/) {
        // Would pattern match on message type
        // For now, just note the mode
        std::cout << "SwitchingActor [Echo]: processing message" << std::endl;
    }

    void handle_message_uppercase_mode(hpactor::TypedMessage&& /*msg*/) {
        // Would pattern match on message type
        std::cout << "SwitchingActor [Uppercase]: processing message" << std::endl;
    }
};

// -----------------------------------------------------------------------------
// Main - demonstrates actor usage patterns
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== HPActor Example 01: Echo Actor ===" << std::endl;

    // Create actor system with configuration
    hpactor::Config config{.scheduler_threads = 4, .max_queue_depth = 1024};
    hpactor::ActorSystem system(config);

    std::cout << "\nNOTE: Actor spawning and message passing are not yet "
                 "implemented.\n"
              << "This example demonstrates the intended API usage patterns.\n"
              << std::endl;

    // -----------------------------------------------------------------
    // Pattern 1: Creating an actor directly (before spawn is implemented)
    // -----------------------------------------------------------------
    // In a complete system:
    //   auto actor = std::make_shared<EchoActor>(context.get(), system);
    //   context->add_child(actor);

    // -----------------------------------------------------------------
    // Pattern 2: Creating actor with ActorSystem
    // -----------------------------------------------------------------
    // When ActorSystem::spawn() is implemented:
    //   auto echo = system.spawn<EchoActor>();
    //   system.send(echo.address(), EchoMessage{"Hello"});
    //   system.send(echo.address(), UppercaseMessage{"hello"});
    //   system.send(echo.address(), ShutdownMessage{});

    // -----------------------------------------------------------------
    // Pattern 3: Behavior switching with become()
    // -----------------------------------------------------------------
    //   auto switching = system.spawn<SwitchingActor>();
    //   system.send(switching.address(), UppercaseMessage{});
    //   system.send(switching.address(), EchoMessage{}); // Now uppercase

    std::cout << "API patterns demonstrated:" << std::endl;
    std::cout << "  1. Subclass EventBasedActor" << std::endl;
    std::cout << "  2. Override make_behavior() to define initial behavior"
              << std::endl;
    std::cout << "  3. Use become() to switch behaviors at runtime" << std::endl;
    std::cout << "  4. std::visit for type-safe message handling" << std::endl;

    return 0;
}
