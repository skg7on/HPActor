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
// Demonstrates actor-to-actor communication patterns:
//
//   - context()->send() for directed messages
//   - context()->reply() for request-response
//   - Multiple actors communicating concurrently
//   - Behavior switching in response to control messages
//   - The link_to() / monitor() API surface (declared, implementations WIP)
//
// Architecture:
//
//   PingActor-1 ──(ping)──> PongActor ──(pong/reply)──> PingActor-1
//   PingActor-2 ──(ping)──> PongActor ──(pong/reply)──> PingActor-2
//                    ^
//                    |  control messages toggle echo/reverse mode
//                   main
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Message type tags
// ---------------------------------------------------------------------------

static const hpactor::TypeTag PingTag{0x00001000};
static const hpactor::TypeTag PongTag{0x00001001};
static const hpactor::TypeTag ReverseModeTag{0x00001002};
static const hpactor::TypeTag EchoModeTag{0x00001003};
static const hpactor::TypeTag StartTag{0x00001004};

// ---------------------------------------------------------------------------
// Payload helpers — encode/decode int as StreamBuffer
// ---------------------------------------------------------------------------

static hpactor::StreamBuffer encode_int(int value) {
    hpactor::StreamBuffer payload(sizeof(int));
    std::memcpy(payload.data(), &value, sizeof(int));
    return payload;
}

static int decode_int(const hpactor::StreamBuffer& payload) {
    if (payload.size() < sizeof(int))
        return 0;
    int value;
    std::memcpy(&value, payload.data(), sizeof(int));
    return value;
}

static hpactor::TypedMessage make_msg(hpactor::TypeTag tag, int value = 0) {
    return hpactor::TypedMessage(tag, encode_int(value));
}

// =============================================================================
// PongActor — receives pings, replies with pongs
// =============================================================================
//
// Demonstrates behavior switching: normal echo mode vs reverse mode.
// In echo mode, pong echoes the count. In reverse mode, pong negates it.
//
// =============================================================================

class PongActor : public hpactor::EventBasedActor {
  public:
    PongActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == PingTag) {
                int count = decode_int(msg.payload());
                int response = reverse_mode_ ? -count : count;
                std::cout << "  PongActor [" << id().value() << "]: ping "
                          << count << " → pong " << response << std::endl;
                context()->reply(make_msg(PongTag, response));
            } else if (msg.type_id() == ReverseModeTag) {
                reverse_mode_ = true;
                std::cout << "  PongActor [" << id().value()
                          << "]: switched to REVERSE mode" << std::endl;
            } else if (msg.type_id() == EchoModeTag) {
                reverse_mode_ = false;
                std::cout << "  PongActor [" << id().value()
                          << "]: switched to ECHO mode" << std::endl;
            }
        }};
    }

  private:
    bool reverse_mode_ = false;
};

// =============================================================================
// PingActor — sends pings to a target, counts responses
// =============================================================================

class PingActor : public hpactor::EventBasedActor {
  public:
    PingActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
              hpactor::ActorAddress target, int rounds, const std::string& name)
        : hpactor::EventBasedActor(ctx, sys), target_(target),
          rounds_remaining_(rounds), name_(name) {
        // Demonstrate link_to API (on AbstractActor, safe to call here).
        // Implementation is a WIP stub, but the API surface is callable.
        link_to(target);
        become(make_behavior());
    }

    // Called after spawn completes (context, scheduler, mailbox are set).
    void on_activate() override {
        // monitor() requires context_, which is set after construction.
        // Safe to call here since the spawn flow wires context first.
        context()->monitor(target_);
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == StartTag) {
                total_rounds_ = rounds_remaining_;
                std::cout << "  " << name_ << " [" << id().value() << "]: starting ("
                          << rounds_remaining_ << " rounds)" << std::endl;
                send_next();
            } else if (msg.type_id() == PongTag) {
                int response = decode_int(msg.payload());
                std::cout << "  " << name_ << " [" << id().value()
                          << "]: received pong " << response << std::endl;
                --rounds_remaining_;
                if (rounds_remaining_ > 0) {
                    send_next();
                } else {
                    std::cout << "  " << name_ << " [" << id().value() << "]: done ("
                              << total_rounds_ << " rounds)" << std::endl;
                }
            }
        }};
    }

  private:
    void send_next() {
        context()->send(target_, make_msg(PingTag, rounds_remaining_));
    }

    hpactor::ActorAddress target_;
    int rounds_remaining_;
    int total_rounds_ = 0;
    std::string name_;
};

// ---------------------------------------------------------------------------
// send_from_main
// ---------------------------------------------------------------------------

static void send_from_main(hpactor::ActorSystem& system, hpactor::ActorId target,
                           hpactor::TypeTag tag, int value = 0) {
    system.deliver_local(target, make_msg(tag, value));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 05: Ping-Pong Communication ===" << std::endl;

    hpactor::Config config{.scheduler_threads = 2,
                           .max_queue_depth = 1024,
                           .cli = {},
                           .mailbox = {},
                           .dead_letters = {},
                           .tracing = {}};
    hpactor::ActorSystem system(config);

    // Spawn PongActor — the shared target
    auto pong = system.spawn<PongActor>();
    std::cout << "Spawned PongActor (id=" << pong.id().value() << ")" << std::endl;

    // Spawn two PingActors targeting the same PongActor
    auto ping1 = system.spawn<PingActor>(pong.address(), 3, "PingActor-1");
    std::cout << "Spawned PingActor-1 (id=" << ping1.id().value()
              << ", rounds=3, linked+monitoring PongActor)" << std::endl;

    auto ping2 = system.spawn<PingActor>(pong.address(), 2, "PingActor-2");
    std::cout << "Spawned PingActor-2 (id=" << ping2.id().value()
              << ", rounds=2, linked+monitoring PongActor)" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ---- Demo 1: Echo mode ping-pong ----
    std::cout << "\n--- Demo 1: Echo-mode ping-pong ---" << std::endl;
    send_from_main(system, ping1.id(), StartTag);
    send_from_main(system, ping2.id(), StartTag);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ---- Demo 2: Reverse mode ----
    std::cout << "\n--- Demo 2: Reverse-mode ping-pong ---" << std::endl;
    auto ping3 = system.spawn<PingActor>(pong.address(), 2, "PingActor-3");
    std::cout << "Spawned PingActor-3 (id=" << ping3.id().value()
              << ", rounds=2)" << std::endl;
    send_from_main(system, pong.id(), ReverseModeTag);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    send_from_main(system, ping3.id(), StartTag);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Switch back to echo mode
    send_from_main(system, pong.id(), EchoModeTag);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ---- Demo 3: API surface — link_to / monitor ----
    std::cout << "\n--- Demo 3: Link/Monitor API ---" << std::endl;
    std::cout << "  link_to() and monitor() are called in PingActor's" << std::endl;
    std::cout << "  constructor. Implementations are WIP stubs, but the"
              << std::endl;
    std::cout << "  API surface is fully declared and callable." << std::endl;

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
