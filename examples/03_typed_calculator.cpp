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
// HPActor Example 03: Typed Actors
// =============================================================================
//
// This example demonstrates hpactor::typed_actor<> for statically typed
// actor references with compile-time message signature validation.
//
// Key concepts demonstrated:
//   - Using typed_actor<Signatures...> for type-safe actor references
//   - Defining typed signatures with hpactor::result<R>(MessageType)
//   - handler_type extraction for compile-time type information
//   - TypedBehavior for statically typed message handlers
//
// NOTE: This example demonstrates the intended API design. The runtime
// infrastructure (spawn, send, typed handler invocation) is not yet functional.
//
// =============================================================================

#include <hpactor/actor/message.hpp>
#include <hpactor/actor/typed_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/typed_behavior.hpp>
#include <iostream>
#include <string>

// -----------------------------------------------------------------------------
// Typed Actor Concepts
// -----------------------------------------------------------------------------
//
// A typed actor is defined with a list of signatures:
//   using MyActor = hpactor::typed_actor<
//       hpactor::result<int>(AddMessage),      // returns int
//       hpactor::result<std::string>(NameMsg)  // returns string
//   >;
//
// Each signature is: hpactor::result<ReturnType>(MessageType)
//   - ReturnType: what the handler returns (use result<void> for void)
//   - MessageType: the message struct to handle
//
// The typed_actor is a reference handle, not the actor itself.
// The actual actor inherits from TypedEventBasedActor<Signatures...>.
//
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Calculator Messages
// -----------------------------------------------------------------------------

struct AddMessage {
    int a;
    int b;
};

struct SubtractMessage {
    int a;
    int b;
};

struct MultiplyMessage {
    int a;
    int b;
};

struct DivideMessage {
    int a;
    int b;
};

struct PowerMessage {
    int base;
    int exponent;
};

struct ShutdownMessage {};

// -----------------------------------------------------------------------------
// Typed Actor Definition
// -----------------------------------------------------------------------------
//
// Define a calculator actor type with typed signatures
// Each signature: result<ReturnType>(MessageType)
using CalculatorActor = hpactor::typed_actor<
    // Addition: takes AddMessage, returns result<int>
    hpactor::result<int>(AddMessage),

    // Subtraction: takes SubtractMessage, returns result<int>
    hpactor::result<int>(SubtractMessage),

    // Multiplication: takes MultiplyMessage, returns result<int>
    hpactor::result<int>(MultiplyMessage),

    // Division: takes DivideMessage, returns result<int>
    // Note: Division can fail (divide by zero)
    hpactor::result<int>(DivideMessage),

    // Power: takes PowerMessage, returns result<int>
    hpactor::result<int>(PowerMessage),

    // Shutdown: takes ShutdownMessage, returns result<void>
    hpactor::result<void>(ShutdownMessage)>;

// -----------------------------------------------------------------------------
// Typed Behavior Handler Extraction
// -----------------------------------------------------------------------------
//
// handler_type is a template that extracts type information from signatures:
//
//   template <typename R, typename Msg>
//   struct handler_type<result<R>(Msg)> {
//     using result = R;    // The return type
//     using message = Msg; // The message type
//   };
//
// This allows compile-time validation and type-safe handler invocation.
//
// -----------------------------------------------------------------------------

void demonstrate_handler_type() {
    std::cout << "handler_type demonstration:" << std::endl;

    // Extract types from a signature
    using AddHandler = hpactor::handler_type<hpactor::result<int>(AddMessage)>;

    // Type aliases for extracted types
    using AddResult = typename AddHandler::result; // int
    using AddMsg = typename AddHandler::message;   // AddMessage

    // These assertions verify the type extraction works correctly
    static_assert(std::is_same_v<AddResult, int>);
    static_assert(std::is_same_v<AddMsg, AddMessage>);

    std::cout << "  result type: int" << std::endl;
    std::cout << "  message type: AddMessage" << std::endl;
}

// -----------------------------------------------------------------------------
// Calculator Implementation
// -----------------------------------------------------------------------------
//
// The actual actor inherits from TypedEventBasedActor with our signatures.
// It implements make_behavior() returning a TypedBehavior.
//
// Pattern:
//
//   class Calculator
//       : public hpactor::TypedEventBasedActor<Signatures...> {
//
//     protected:
//       behavior_type make_behavior() override {
//         behavior_type bh;
//
//         // Register handler for AddMessage
//         bh([](AddMessage msg) -> hpactor::result<int> {
//           return hpactor::result<int>::make(msg.a + msg.b);
//         });
//
//         // Register other handlers...
//         return bh;
//       }
//
//     public:
//       Calculator(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
//         : TypedEventBasedActor(ctx, sys) {}
//   };
//
// NOTE: TypedBehavior handlers are currently stubs - invoke() and matches()
// return false/void. This example shows the intended API design.
//
// -----------------------------------------------------------------------------

class Calculator
    : public hpactor::TypedEventBasedActor<
          // Forward our signatures to TypedEventBasedActor
          hpactor::result<int>(AddMessage), hpactor::result<int>(SubtractMessage),
          hpactor::result<int>(MultiplyMessage), hpactor::result<int>(DivideMessage),
          hpactor::result<int>(PowerMessage), hpactor::result<void>(ShutdownMessage)> {
  protected:
    // TypedBehavior with handlers for each signature
    behavior_type make_behavior() override {
        behavior_type bh;

        // Register handler for AddMessage
        bh([](AddMessage msg) -> hpactor::result<int> {
            return hpactor::result<int>::make(msg.a + msg.b);
        });

        // Register handler for SubtractMessage
        bh([](SubtractMessage msg) -> hpactor::result<int> {
            return hpactor::result<int>::make(msg.a - msg.b);
        });

        // Register handler for MultiplyMessage
        bh([](MultiplyMessage msg) -> hpactor::result<int> {
            return hpactor::result<int>::make(msg.a * msg.b);
        });

        // Register handler for DivideMessage
        bh([](DivideMessage msg) -> hpactor::result<int> {
            if (msg.b == 0) {
                return hpactor::result<int>::make(hpactor::error{1, "divide by "
                                                                    "zero"});
            }
            return hpactor::result<int>::make(msg.a / msg.b);
        });

        // Register handler for PowerMessage
        bh([](PowerMessage msg) -> hpactor::result<int> {
            int power_result = 1;
            for (int i = 0; i < msg.exponent; ++i) {
                power_result *= msg.base;
            }
            return hpactor::result<int>::make(std::move(power_result));
        });

        // Register handler for ShutdownMessage
        bh([](ShutdownMessage) -> hpactor::result<void> {
            return hpactor::result<void>::make();
        });

        return bh;
    }

  public:
    Calculator(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : TypedEventBasedActor(ctx, sys) {}
};

// -----------------------------------------------------------------------------
// Main - demonstrates typed actor usage
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== HPActor Example 03: Typed Actors ===" << std::endl;

    demonstrate_handler_type();

    hpactor::Config config{.scheduler_threads = 4, .max_queue_depth = 1024};
    hpactor::ActorSystem system(config);

    std::cout << "\nNOTE: Actor spawning and typed handler invocation are not "
                 "yet "
                 "fully implemented.\n"
              << "This example demonstrates the intended API usage patterns.\n"
              << std::endl;

    std::cout << "\nTyped actor pattern:" << std::endl;
    std::cout << "  1. Define signatures: typed_actor<result<R>(Msg), ...>"
              << std::endl;
    std::cout << "  2. Inherit from TypedEventBasedActor<Signatures...>"
              << std::endl;
    std::cout << "  3. Register handlers in make_behavior() via bh([](Msg){})"
              << std::endl;
    std::cout << "  4. Use typed_actor<...> as a type-safe reference handle"
              << std::endl;
    std::cout << std::endl;

    // -----------------------------------------------------------------
    // Pattern: Using typed actors with typed messages
    // -----------------------------------------------------------------
    // In a complete system:
    //
    //   // Create typed actor reference
    //   CalculatorActor calc = system.spawn<Calculator>();
    //
    //   // Type-safe message sending - compiler enforces correct message types
    //   calc(AddMessage{10, 5});           // Sends AddMessage
    //   calc(SubtractMessage{10, 5});      // Sends SubtractMessage
    //   calc(MultiplyMessage{10, 5});      // Sends MultiplyMessage
    //   calc(DivideMessage{10, 0});        // Returns error
    //
    //   // Wrong message type - compiler error:
    //   calc("hello");                     // Error: no overload for const
    //   char*

    std::cout << "Signature format:" << std::endl;
    std::cout << "  hpactor::result<ReturnType>(MessageType)" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  hpactor::result<int>(AddMessage)     // returns int"
              << std::endl;
    std::cout << "  hpactor::result<void>(Shutdown)       // returns void"
              << std::endl;
    std::cout << "  hpactor::result<std::string>(NameMsg) // returns string"
              << std::endl;

    return 0;
}
