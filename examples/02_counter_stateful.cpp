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
// HPActor Example 02: Stateful Actor
// =============================================================================
//
// Demonstrates StatefulActor<T> — an EventBasedActor with explicit state that
// persists across message handlers. Uses the behavior-based dispatch path
// (the default; StatefulActor mandates make_behavior()).
//
// Key concepts:
//   - Subclassing hpactor::StatefulActor<T> with a custom state struct
//   - Accessing state via state() and state() const
//   - Typed message dispatch in make_behavior()
//   - context()->reply() to respond to the sender
//
// =============================================================================

#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cstring>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Message type tags
// ---------------------------------------------------------------------------

static const hpactor::TypeTag IncrementTag{200};
static const hpactor::TypeTag DecrementTag{201};
static const hpactor::TypeTag ResetTag{202};
static const hpactor::TypeTag GetValueTag{203};
static const hpactor::TypeTag SetValueTag{204};

// ---------------------------------------------------------------------------
// Payload helpers — encode/decode ints as bytes
// ---------------------------------------------------------------------------

static hpactor::bytes encode_int(int value) {
    hpactor::bytes payload(sizeof(int));
    std::memcpy(payload.data(), &value, sizeof(int));
    return payload;
}

static int decode_int(const hpactor::bytes& payload) {
    if (payload.size() < sizeof(int)) return 0;
    int value;
    std::memcpy(&value, payload.data(), sizeof(int));
    return value;
}

static hpactor::TypedMessage make_msg(hpactor::TypeTag tag, int value = 0) {
    return hpactor::TypedMessage(tag, encode_int(value));
}

// =============================================================================
// CounterState — state for CounterActor
// =============================================================================

struct CounterState {
    int value = 0;
    int max_value = 100;
    int min_value = 0;
};

// =============================================================================
// CounterActor — increment, decrement, reset, get-value with bounds checking
// =============================================================================

class CounterActor : public hpactor::StatefulActor<CounterState> {
  public:
    CounterActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                 int initial = 0, int min_val = 0, int max_val = 100)
        : hpactor::StatefulActor<CounterState>(ctx, sys) {
        state().value = initial;
        state().min_value = min_val;
        state().max_value = max_val;
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == IncrementTag) {
                int delta = decode_int(msg.payload());
                if (delta <= 0) delta = 1;
                state().value += delta;
                if (state().value > state().max_value)
                    state().value = state().max_value;
                std::cout << "  CounterActor [" << id().value()
                          << "]: +" << delta << " → " << state().value
                          << std::endl;
            } else if (msg.type_id() == DecrementTag) {
                int delta = decode_int(msg.payload());
                if (delta <= 0) delta = 1;
                state().value -= delta;
                if (state().value < state().min_value)
                    state().value = state().min_value;
                std::cout << "  CounterActor [" << id().value()
                          << "]: -" << delta << " → " << state().value
                          << std::endl;
            } else if (msg.type_id() == ResetTag) {
                state().value = 0;
                std::cout << "  CounterActor [" << id().value()
                          << "]: reset → 0" << std::endl;
            } else if (msg.type_id() == GetValueTag) {
                std::cout << "  CounterActor [" << id().value()
                          << "]: value = " << state().value << std::endl;
                context()->reply(make_msg(GetValueTag, state().value));
            }
        }};
    }
};

// =============================================================================
// GaugeState — state for GaugeActor
// =============================================================================

struct GaugeState {
    double value = 0.0;
    double max_value = 100.0;
};

// =============================================================================
// GaugeActor — set-value and bounded increment, demonstrates different state
// =============================================================================

class GaugeActor : public hpactor::StatefulActor<GaugeState> {
  public:
    GaugeActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
               double initial = 0.0, double max_val = 100.0)
        : hpactor::StatefulActor<GaugeState>(ctx, sys) {
        state().value = initial;
        state().max_value = max_val;
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == IncrementTag) {
                double delta = static_cast<double>(decode_int(msg.payload()));
                state().value += delta;
                if (state().value > state().max_value)
                    state().value = state().max_value;
                std::cout << "  GaugeActor [" << id().value()
                          << "]: +" << delta << " → " << state().value
                          << std::endl;
            } else if (msg.type_id() == SetValueTag) {
                double new_val = static_cast<double>(decode_int(msg.payload()));
                state().value = new_val;
                if (state().value > state().max_value)
                    state().value = state().max_value;
                std::cout << "  GaugeActor [" << id().value()
                          << "]: set → " << state().value << std::endl;
            } else if (msg.type_id() == GetValueTag) {
                std::cout << "  GaugeActor [" << id().value()
                          << "]: value = " << state().value << std::endl;
                context()->reply(make_msg(GetValueTag,
                                          static_cast<int>(state().value)));
            }
        }};
    }
};

// ---------------------------------------------------------------------------
// send_from_main — enqueue a message from outside the actor system
// ---------------------------------------------------------------------------

static void send_from_main(hpactor::ActorSystem& system,
                           hpactor::ActorId target, hpactor::TypeTag tag,
                           int value = 0) {
    system.deliver_local(target, make_msg(tag, value));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 02: Stateful Actor ===" << std::endl;

    hpactor::Config config{.scheduler_threads = 2, .max_queue_depth = 1024};
    hpactor::ActorSystem system(config);

    // Spawn actors with initial state
    auto counter = system.spawn<CounterActor>(10, 0, 100);
    std::cout << "Spawned CounterActor (id=" << counter.id().value()
              << ", initial=10, bounds=[0,100])" << std::endl;

    auto gauge = system.spawn<GaugeActor>(50.0, 100.0);
    std::cout << "Spawned GaugeActor (id=" << gauge.id().value()
              << ", initial=50.0, max=100.0)" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ---- Demo 1: CounterActor operations ----
    std::cout << "\n--- Demo 1: CounterActor ---" << std::endl;
    send_from_main(system, counter.id(), IncrementTag, 5);
    send_from_main(system, counter.id(), IncrementTag, 3);
    send_from_main(system, counter.id(), DecrementTag, 2);
    send_from_main(system, counter.id(), GetValueTag);
    send_from_main(system, counter.id(), ResetTag);
    send_from_main(system, counter.id(), GetValueTag);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Demo 2: GaugeActor operations ----
    std::cout << "\n--- Demo 2: GaugeActor ---" << std::endl;
    send_from_main(system, gauge.id(), IncrementTag, 25);
    send_from_main(system, gauge.id(), IncrementTag, 30);
    send_from_main(system, gauge.id(), GetValueTag);
    send_from_main(system, gauge.id(), SetValueTag, 10);
    send_from_main(system, gauge.id(), IncrementTag, 5);
    send_from_main(system, gauge.id(), GetValueTag);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
