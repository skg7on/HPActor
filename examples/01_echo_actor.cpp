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
// Demonstrates the core actor patterns in HPActor:
//
//   - Subclassing EventBasedActor
//   - ActorSystem::spawn<T>() to create actors
//   - context()->send() for actor-to-actor messaging
//   - context()->reply() to respond to the sender
//   - Dynamic behavior switching (become() / coroutine flag)
//
// Two dispatch paths exist, selected at runtime via Config::use_coroutines:
//
//   Coroutine path (requires HPACTOR_SUPPORT_COROUTINES=1 at compile time):
//     Override act() — a C++20 coroutine that co_awaits messages.
//     Enable with: config.use_coroutines = true (default: false).
//
//   Behavior path (always available, the default):
//     Override make_behavior() — a callback invoked by receive().
//     Use become() for runtime behavior switching.
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>

#if HPACTOR_SUPPORT_COROUTINES
#include <hpactor/sched/coroutine_awaiters.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Custom message type tags
// ---------------------------------------------------------------------------

static const hpactor::TypeTag EchoMsgTag{100};
static const hpactor::TypeTag UppercaseMsgTag{101};

// ---------------------------------------------------------------------------
// String message helpers
// ---------------------------------------------------------------------------

static hpactor::TypedMessage make_string_msg(hpactor::TypeTag tag,
                                             const std::string& text) {
    hpactor::bytes payload(text.begin(), text.end());
    return hpactor::TypedMessage(tag, std::move(payload));
}

static std::string extract_string(const hpactor::bytes& payload) {
    return {payload.begin(), payload.end()};
}

// =============================================================================
// EchoActor — receives messages and echoes back to sender via reply()
// =============================================================================

class EchoActor : public hpactor::EventBasedActor {
  public:
    EchoActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
#if HPACTOR_SUPPORT_COROUTINES
        if (!sys.use_coroutines()) {
            become(make_behavior());
        }
#else
        become(make_behavior());
#endif
    }

#if HPACTOR_SUPPORT_COROUTINES
    hpactor::sched::CoroutineTask act() override {
        std::cout << "  EchoActor [" << id().value() << "]: started"
                  << std::endl;
        int count = 0;
        while (count < 4) {
            auto msg = co_await make_mailbox_awaiter();
            if (msg.type_id() == EchoMsgTag) {
                auto text = extract_string(msg.payload());
                std::cout << "  EchoActor [" << id().value()
                          << "]: received \"" << text << "\"" << std::endl;
                // Reply to sender. current_sender must be set manually in the
                // coroutine path (receive() does it automatically in the
                // behavior path).
                context()->set_current_sender(msg.sender_address());
                context()->reply(
                    make_string_msg(EchoMsgTag, "echo: " + text));
                ++count;
            }
        }
        std::cout << "  EchoActor [" << id().value() << "]: done ("
                  << count << " messages)" << std::endl;
        co_return;
    }
#else
  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == EchoMsgTag) {
                std::cout << "  EchoActor [" << id().value()
                          << "]: received \""
                          << extract_string(msg.payload()) << "\""
                          << std::endl;
                context()->reply(
                    make_string_msg(EchoMsgTag, "echo: " +
                                     extract_string(msg.payload())));
            }
        }};
    }
#endif
};

// =============================================================================
// RelayActor — forwards messages to a target via context()->send()
// =============================================================================

class RelayActor : public hpactor::EventBasedActor {
  public:
    RelayActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
               hpactor::ActorAddress target)
        : hpactor::EventBasedActor(ctx, sys), target_(target) {
#if HPACTOR_SUPPORT_COROUTINES
        if (!sys.use_coroutines()) {
            become(make_behavior());
        }
#else
        become(make_behavior());
#endif
    }

#if HPACTOR_SUPPORT_COROUTINES
    hpactor::sched::CoroutineTask act() override {
        std::cout << "  RelayActor [" << id().value() << "]: started"
                  << std::endl;
        int count = 0;
        while (count < 2) {
            auto msg = co_await make_mailbox_awaiter();
            if (msg.type_id() == EchoMsgTag) {
                auto text = extract_string(msg.payload());
                std::cout << "  RelayActor [" << id().value()
                          << "]: forwarding \"" << text << "\"" << std::endl;
                context()->send(target_,
                                make_string_msg(EchoMsgTag,
                                                "relayed: " + text));
                ++count;
            }
        }
        std::cout << "  RelayActor [" << id().value() << "]: done ("
                  << count << " messages)" << std::endl;
        co_return;
    }
#else
  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == EchoMsgTag) {
                auto text = extract_string(msg.payload());
                std::cout << "  RelayActor [" << id().value()
                          << "]: forwarding \"" << text << "\"" << std::endl;
                context()->send(
                    target_,
                    make_string_msg(EchoMsgTag, "relayed: " + text));
            }
        }};
    }
#endif

  private:
    hpactor::ActorAddress target_;
};

// =============================================================================
// SwitchingActor — dynamic behavior switching
// =============================================================================

class SwitchingActor : public hpactor::EventBasedActor {
  public:
    SwitchingActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
#if HPACTOR_SUPPORT_COROUTINES
        if (!sys.use_coroutines()) {
            become(make_behavior());
        }
#else
        become(make_behavior());
#endif
    }

#if HPACTOR_SUPPORT_COROUTINES
    hpactor::sched::CoroutineTask act() override {
        std::cout << "  SwitchingActor [" << id().value()
                  << "]: started in echo mode" << std::endl;
        bool uppercase_mode = false;
        while (true) {
            auto msg = co_await make_mailbox_awaiter();
            if (msg.type_id() == UppercaseMsgTag) {
                std::cout << "  SwitchingActor [" << id().value()
                          << "]: switching to UPPERCASE mode" << std::endl;
                uppercase_mode = true;
            } else if (msg.type_id() == EchoMsgTag) {
                auto text = extract_string(msg.payload());
                if (uppercase_mode) {
                    std::string upper = text;
                    std::transform(upper.begin(), upper.end(), upper.begin(),
                                   [](unsigned char c) {
                                       return std::toupper(c);
                                   });
                    std::cout << "  SwitchingActor [" << id().value()
                              << "]: \"" << text << "\" → \""
                              << upper << "\"" << std::endl;
                    context()->set_current_sender(msg.sender_address());
                    context()->reply(
                        make_string_msg(EchoMsgTag, "[UPPER] " + upper));
                } else {
                    std::cout << "  SwitchingActor [" << id().value()
                              << "]: received \"" << text << "\""
                              << std::endl;
                    context()->set_current_sender(msg.sender_address());
                    context()->reply(
                        make_string_msg(EchoMsgTag, "[echo] " + text));
                }
            }
        }
        co_return;
    }
#else
  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == UppercaseMsgTag) {
                std::cout << "  SwitchingActor [" << id().value()
                          << "]: switching to UPPERCASE mode" << std::endl;
                become_uppercase();
            } else if (msg.type_id() == EchoMsgTag) {
                auto text = extract_string(msg.payload());
                std::cout << "  SwitchingActor [" << id().value()
                          << "]: received \"" << text << "\"" << std::endl;
                context()->reply(
                    make_string_msg(EchoMsgTag, "[echo] " + text));
            }
        }};
    }

  private:
    void become_uppercase() {
        become(hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == EchoMsgTag) {
                auto text = extract_string(msg.payload());
                std::string upper = text;
                std::transform(upper.begin(), upper.end(), upper.begin(),
                               [](unsigned char c) {
                                   return std::toupper(c);
                               });
                std::cout << "  SwitchingActor [" << id().value()
                          << "]: \"" << text << "\" → \""
                          << upper << "\"" << std::endl;
                context()->reply(
                    make_string_msg(EchoMsgTag, "[UPPER] " + upper));
            }
        }});
    }
#endif
};

// ---------------------------------------------------------------------------
// send_from_main
// ---------------------------------------------------------------------------

static void send_from_main(hpactor::ActorSystem& system,
                           hpactor::ActorId target, hpactor::TypeTag tag,
                           const std::string& text) {
    system.deliver_local(target, make_string_msg(tag, text));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 01: Echo Actor ===" << std::endl;

    hpactor::Config config{.scheduler_threads = 2, .max_queue_depth = 1024};
    // config.use_coroutines = true;  // uncomment to enable C++20 coroutine dispatch
    hpactor::ActorSystem system(config);

#if HPACTOR_SUPPORT_COROUTINES
    if (config.use_coroutines) {
        std::cout << "Dispatch: C++20 coroutines\n" << std::endl;
    } else {
        std::cout << "Dispatch: Behavior callbacks (become/make_behavior)\n"
                  << std::endl;
    }
#else
    std::cout << "Dispatch: Behavior callbacks (become/make_behavior)\n"
              << std::endl;
#endif

    // Spawn actors
    auto echo = system.spawn<EchoActor>();
    std::cout << "Spawned EchoActor (id=" << echo.id().value() << ")"
              << std::endl;

    auto relay = system.spawn<RelayActor>(echo.address());
    std::cout << "Spawned RelayActor (id=" << relay.id().value()
              << ", target=EchoActor)" << std::endl;

    auto switching = system.spawn<SwitchingActor>();
    std::cout << "Spawned SwitchingActor (id=" << switching.id().value() << ")"
              << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ---- Demo 1: Direct messaging + reply ----
    // Send all messages upfront so the first notify_ready drains them all
    // in a single execute_actor call (the behavior path re-enqueues while
    // the mailbox is non-empty).
    std::cout << "\n--- Demo 1: EchoActor direct ---" << std::endl;
    send_from_main(system, echo.id(), EchoMsgTag, "hello");
    send_from_main(system, echo.id(), EchoMsgTag, "world");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Demo 2: Relay (context->send) ----
    std::cout << "\n--- Demo 2: RelayActor → EchoActor ---" << std::endl;
    send_from_main(system, relay.id(), EchoMsgTag, "alpha");
    send_from_main(system, relay.id(), EchoMsgTag, "beta");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Demo 3: Behavior switching ----
    std::cout << "\n--- Demo 3: SwitchingActor mode toggle ---" << std::endl;
    send_from_main(system, switching.id(), EchoMsgTag, "quiet");
    send_from_main(system, switching.id(), UppercaseMsgTag, "");
    send_from_main(system, switching.id(), EchoMsgTag, "LOUD");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
