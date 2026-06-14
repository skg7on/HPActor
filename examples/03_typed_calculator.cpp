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
// HPActor Example 03: Typed Calculator
// =============================================================================
//
// Demonstrates TypedEventBasedActor with compile-time type-safe messaging:
//
//   - Typed message structs (no protobuf needed for local messages)
//   - TypedBehavior<Signatures...> with handler registration via on()
//   - TypedEventBasedActorRef<Signatures...> handle — statically checked at
//   compile time
//   - result<T> return values from handlers
//   - Compile-time prevention of sending wrong message types
//
// The typed actor dispatch goes through operator()(T&&), not the scheduler
// mailbox. This is a direct, type-safe call path for local actors.
//
// =============================================================================

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/typed_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <cmath>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Message types — plain structs, no protobuf needed
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Calculator actor type alias
// ---------------------------------------------------------------------------

using calculator_handle = hpactor::TypedEventBasedActorRef<
    hpactor::result<int>(AddMessage), hpactor::result<int>(SubtractMessage),
    hpactor::result<int>(MultiplyMessage), hpactor::result<int>(DivideMessage),
    hpactor::result<int>(PowerMessage), hpactor::result<void>(ShutdownMessage)>;

// =============================================================================
// CalculatorActor — typed actor with arithmetic operations
// =============================================================================

class CalculatorActor
    : public hpactor::TypedEventBasedActor<
          hpactor::result<int>(AddMessage), hpactor::result<int>(SubtractMessage),
          hpactor::result<int>(MultiplyMessage), hpactor::result<int>(DivideMessage),
          hpactor::result<int>(PowerMessage), hpactor::result<void>(ShutdownMessage)> {
  public:
    using base_type = hpactor::TypedEventBasedActor<
        hpactor::result<int>(AddMessage), hpactor::result<int>(SubtractMessage),
        hpactor::result<int>(MultiplyMessage), hpactor::result<int>(DivideMessage),
        hpactor::result<int>(PowerMessage), hpactor::result<void>(ShutdownMessage)>;

    CalculatorActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : base_type(ctx, sys) {
        become(make_behavior());
    }

    int operation_count() const {
        return operation_count_;
    }
    bool is_shutdown() const {
        return shutdown_;
    }

  protected:
    typename base_type::behavior_type make_behavior() override {
        typename base_type::behavior_type bh;
        bh.on([](AddMessage msg) -> int { return msg.a + msg.b; });
        bh.on([](SubtractMessage msg) -> int { return msg.a - msg.b; });
        bh.on([](MultiplyMessage msg) -> int { return msg.a * msg.b; });
        bh.on([](DivideMessage msg) -> int {
            if (msg.b == 0)
                return 0;
            return msg.a / msg.b;
        });
        bh.on([](PowerMessage msg) -> int {
            return static_cast<int>(std::pow(msg.base, msg.exponent));
        });
        bh.on([this](ShutdownMessage) { shutdown_ = true; });
        return bh;
    }

  private:
    int operation_count_ = 0;
    bool shutdown_ = false;
};

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 03: Typed Calculator ===" << std::endl;

    // ---- Compile-time type checking ----
    std::cout << "\n--- Compile-time type verification ---" << std::endl;

    // Verify handler types at compile time
    static_assert(
        std::is_same_v<hpactor::handler_type<hpactor::result<int>(AddMessage)>::result, int>);
    static_assert(
        std::is_same_v<hpactor::handler_type<hpactor::result<int>(AddMessage)>::message,
                       AddMessage>);
    static_assert(
        std::is_same_v<hpactor::handler_type<hpactor::result<void>(ShutdownMessage)>::result,
                       void>);
    std::cout << "  handler_type traits verified" << std::endl;

    // Verify TypedEventBasedActorRef handle type
    static_assert(sizeof(calculator_handle) > 0);
    std::cout << "  calculator_handle type verified" << std::endl;

    // ---- Runtime: spawn and direct typed dispatch ----
    std::cout << "\n--- Runtime typed dispatch ---" << std::endl;

    hpactor::Config config{.scheduler_threads = 1,
                           .max_queue_depth = 1024,
                           .cli = {},
                           .mailbox = {},
                           .dead_letters = {},
                           .tracing = {},
                           .process = {}};
    hpactor::ActorSystem system(config);

    // Spawn the calculator
    auto calc = system.spawn<CalculatorActor>();
    std::cout << "Spawned CalculatorActor (id=" << calc.id().value() << ")"
              << std::endl;

    // Get a type-safe handle.
    // The TypedEventBasedActorRef handle constrains what messages can be sent —
    // calling calc_handle(WrongType{}) would be a compile error.
    auto actor_ptr =
        std::static_pointer_cast<CalculatorActor>(system.get_actor(calc.id()));
    calculator_handle calc_handle(actor_ptr);

    // Invoke operations through the typed handle
    auto r1 = (*actor_ptr)(AddMessage{10, 5});
    std::cout << "  add(10, 5) = " << r1.value() << std::endl;

    auto r2 = (*actor_ptr)(SubtractMessage{10, 5});
    std::cout << "  subtract(10, 5) = " << r2.value() << std::endl;

    auto r3 = (*actor_ptr)(MultiplyMessage{10, 5});
    std::cout << "  multiply(10, 5) = " << r3.value() << std::endl;

    auto r4 = (*actor_ptr)(DivideMessage{10, 3});
    std::cout << "  divide(10, 3) = " << r4.value() << std::endl;

    auto r5 = (*actor_ptr)(PowerMessage{2, 10});
    std::cout << "  power(2, 10) = " << r5.value() << std::endl;

    auto r6 = (*actor_ptr)(DivideMessage{5, 0});
    std::cout << "  divide(5, 0) = " << r6.value() << " (division by zero → 0)"
              << std::endl;

    (*actor_ptr)(ShutdownMessage{});
    std::cout << "  shutdown = " << (actor_ptr->is_shutdown() ? "true" : "false")
              << std::endl;

    // ---- API summary ----
    std::cout << "\n--- Typed actor API reference ---" << std::endl;
    std::cout << "  TypedEventBasedActor<Signatures...>" << std::endl;
    std::cout << "    make_behavior() → TypedBehavior<Signatures...>" << std::endl;
    std::cout << "    operator()(T&&) → result<R>  // typed dispatch" << std::endl;
    std::cout << "  TypedBehavior<Signatures...>" << std::endl;
    std::cout << "    on(F&& handler)  // register handler for message type"
              << std::endl;
    std::cout << "  TypedEventBasedActorRef<Signatures...>  // type-safe handle"
              << std::endl;
    std::cout << "    operator()(T&&)  // compile-time checked send" << std::endl;

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
