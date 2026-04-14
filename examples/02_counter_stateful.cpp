// =============================================================================
// HPActor Example 02: Stateful Actor
// =============================================================================
//
// This example demonstrates how to use hpactor::StatefulActor to manage
// explicit state that persists across message handlers.
//
// Key concepts demonstrated:
//   - Subclassing hpactor::StatefulActor<T> with a custom state struct
//   - Accessing state via state() and state() const accessors
//   - State persistence across message handlers
//   - Multiple message types for different operations on state
//
// NOTE: This example demonstrates the intended API design. The runtime
// infrastructure (spawn, send, scheduler) is not yet functional.
//
// =============================================================================

#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/actor/message.hpp>
#include <iostream>
#include <string>

// -----------------------------------------------------------------------------
// State Struct Definition
// -----------------------------------------------------------------------------
//
// The state struct is the single source of truth for actor state.
// It can contain any data types and is accessed via StatefulActor::state().
//
// Pattern:
//   struct MyState {
//     int counter = 0;
//     std::string name;
//     std::vector<int> history;
//   };
//
//   class MyActor : public hpactor::StatefulActor<MyState> {
//     protected:
//       hpactor::Behavior make_behavior() override { ... }
//   };
//
// -----------------------------------------------------------------------------

struct CounterState {
    int value = 0;         // Current counter value
    int max_value = 100;   // Upper bound (prevent overflow)
    int min_value = 0;     // Lower bound
    std::string name;      // Optional name for debugging
};

// -----------------------------------------------------------------------------
// Message Definitions
// -----------------------------------------------------------------------------

struct IncrementMessage {
    int delta = 1;  // Amount to increment (default: 1)
};

struct DecrementMessage {
    int delta = 1;  // Amount to decrement (default: 1)
};

struct ResetMessage {};

struct GetValueMessage {};

struct SetBoundsMessage {
    int min_value;
    int max_value;
};

// -----------------------------------------------------------------------------
// CounterActor - demonstrates StatefulActor with explicit state
// -----------------------------------------------------------------------------
//
// StatefulActor<T> is an EventBasedActor with an explicit state object.
// State persists across message handlers - unlike implicit member variables,
// the state is clearly delimited and can be inspected/dumped easily.
//
// Pattern:
//   class CounterActor : public hpactor::StatefulActor<CounterState> {
//     protected:
//       hpactor::Behavior make_behavior() override {
//         return hpactor::Behavior{[this](hpactor::MessageVariant&& msg) {
//           // Access state via state().member
//         }};
//       }
//     public:
//       CounterActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
//         : hpactor::StatefulActor<CounterState>(ctx, sys) {
//         state().name = "counter";  // Initialize state
//       }
//   };
//
// -----------------------------------------------------------------------------

// Note: StatefulActor doesn't properly inherit constructors from EventBasedActor.
// This is a known issue in the framework. Once fixed, the constructor would be:
//
//   CounterActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
//       : hpactor::StatefulActor<CounterState>(ctx, sys) {
//       // Initialize state
//       state().name = "counter";
//       state().value = 0;
//       state().min_value = 0;
//       state().max_value = 100;
//   }
//
// For now, the pattern is demonstrated without direct instantiation.

class CounterActor /*: public hpactor::StatefulActor<CounterState>*/ {
  // protected:
  //   hpactor::Behavior make_behavior() override {
  //       return hpactor::Behavior{[this](hpactor::MessageVariant&& /*msg*/) {
  //           // In a real implementation, would use std::visit to handle messages
  //           // and modify state via state().member = value
  //           std::cout << "CounterActor [" << state().name << "] handling message"
  //                     << std::endl;
  //       }};
  //   }

  public:
    CounterActor(/*hpactor::ActorContext* ctx, hpactor::ActorSystem& sys*/) {
        // When StatefulActor constructor inheritance is fixed:
        // : hpactor::StatefulActor<CounterState>(ctx, sys) {
        // Initialize state
        name_ = "counter";
        value_ = 0;
        min_value_ = 0;
        max_value_ = 100;
    }

    // Demo state accessors (would be state() in real implementation)
    int value() const { return value_; }
    int max_value() const { return max_value_; }
    int min_value() const { return min_value_; }
    const std::string& name() const { return name_; }

    void set_value(int v) { value_ = v; }
    void increment(int delta) { value_ += delta; }
    void decrement(int delta) { value_ -= delta; }
    void reset() { value_ = 0; }

  private:
    // These would be part of CounterState in real StatefulActor
    std::string name_;
    int value_ = 0;
    int max_value_ = 100;
    int min_value_ = 0;

    // Message handlers would look like:
    //
    // void handle(IncrementMessage msg) {
    //   state().value += msg.delta;
    //   if (state().value > state().max_value) {
    //     state().value = state().max_value;
    //   }
    // }
};

// -----------------------------------------------------------------------------
// GaugeActor - demonstrates different state with same pattern
// -----------------------------------------------------------------------------

struct GaugeState {
    double value = 0.0;
    double max_value = 1000.0;
    std::string unit;
};

// Note: Same constructor inheritance issue as CounterActor
// When framework is fixed:
//   class GaugeActor : public hpactor::StatefulActor<GaugeState> { ... }

class GaugeActor {
  public:
    GaugeActor() {
        unit_ = "percent";
        value_ = 0.0;
        max_value_ = 100.0;
    }

    double value() const { return value_; }
    double max_value() const { return max_value_; }
    const std::string& unit() const { return unit_; }

    void set_value(double v) { value_ = v; }
    void increment(double delta) {
        value_ += delta;
        if (value_ > max_value_) value_ = max_value_;
    }
    void decrement(double delta) {
        value_ -= delta;
        if (value_ < 0) value_ = 0;
    }
    void reset() { value_ = 0.0; }

  private:
    std::string unit_;
    double value_ = 0.0;
    double max_value_ = 100.0;
};

// -----------------------------------------------------------------------------
// Main - demonstrates stateful actor usage
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== HPActor Example 02: Stateful Actor ===" << std::endl;

    hpactor::Config config{
        .scheduler_threads = 4,
        .max_queue_depth = 1024
    };
    hpactor::ActorSystem system(config);

    std::cout << "\nNOTE: Actor spawning and message passing are not yet "
                 "implemented.\n"
              << "This example demonstrates the intended API usage patterns.\n"
              << std::endl;

    std::cout << "StatefulActor pattern:" << std::endl;
    std::cout << "  1. Define a state struct (CounterState, GaugeState)" << std::endl;
    std::cout << "  2. Subclass StatefulActor<YourStateStruct>" << std::endl;
    std::cout << "  3. Access state via state() and state() const" << std::endl;
    std::cout << "  4. State persists across message handlers" << std::endl;
    std::cout << std::endl;

    // -----------------------------------------------------------------
    // Pattern: Using StatefulActor with custom state
    // -----------------------------------------------------------------
    // In a complete system:
    //
    //   auto counter = system.spawn<CounterActor>();
    //
    //   // Counter starts at 0
    //   system.send(counter.address(), IncrementMessage{10});  // value = 10
    //   system.send(counter.address(), IncrementMessage{5});   // value = 15
    //   system.send(counter.address(), DecrementMessage{3});  // value = 12
    //   system.send(counter.address(), GetValueMessage{});     // prints 12
    //   system.send(counter.address(), ResetMessage{});       // value = 0
    //
    //   // With bounds
    //   system.send(counter.address(), SetBoundsMessage{0, 100});
    //   system.send(counter.address(), IncrementMessage{200}); // clamped to 100

    std::cout << "Key API:" << std::endl;
    std::cout << "  StatefulActor<StateStruct>::state() -> T&" << std::endl;
    std::cout << "  StatefulActor<StateStruct>::state() const -> const T&" << std::endl;
    std::cout << "  state().member  // Direct access to state fields" << std::endl;

    return 0;
}
