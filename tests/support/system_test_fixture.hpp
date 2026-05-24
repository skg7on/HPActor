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

// Shared test utilities for HPActor system tests.
// Provides helper Config builders, reusable test actors, and polling helpers.

#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace hpactor::test {

// ── Config builders
// ───────────────────────────────────────────────────────────

inline Config minimal_config() {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.tracing.enabled = false;
    return cfg;
}

inline Config config_with_scheduler(size_t threads = 1) {
    Config cfg;
    cfg.scheduler_threads = threads;
    cfg.enable_network = false;
    cfg.cli.enabled = false;
    cfg.tracing.enabled = false;
    return cfg;
}

inline Config config_with_tracing() {
    Config cfg = minimal_config();
    cfg.tracing.enabled = true;
    cfg.tracing.sampler = tracing::SamplerKind::kAlwaysOn;
    cfg.tracing.exporter = tracing::TraceExporterKind::kMemory;
    return cfg;
}

// ── assert_eventually — poll a predicate with a deadline
// ──────────────────────

inline bool
assert_eventually(std::function<bool()> predicate, int deadline_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(deadline_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// ── CountingActor — records every TypedMessage received
// ───────────────────────

class CountingActor : public EventBasedActor, public LifecycleActor {
  public:
    std::vector<uint32_t> received_type_ids;
    int handler_count = 0;
    bool system_init_received = false;

    CountingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            handler_count++;
            received_type_ids.push_back(static_cast<uint32_t>(msg.type_id()));
            if (static_cast<uint32_t>(msg.type_id()) == 12) {
                system_init_received = true;
            }
        }};
    }
};

// ── PingActor — replies to messages by echoing the type tag value
// ─────────────

class EchoActor : public EventBasedActor, public LifecycleActor {
  public:
    int messages_handled = 0;

    EchoActor(ActorContext* ctx, ActorSystem& sys) : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            messages_handled++;
            (void)msg;
        }};
    }
};

// ── FailingActor — fails after N messages or on command
// ───────────────────────

class FailingActor : public EventBasedActor, public LifecycleActor {
  public:
    int messages_processed = 0;
    int fail_after = -1; // -1 = never fail automatically

    FailingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& /*msg*/) {
            messages_processed++;
            if (fail_after > 0 && messages_processed >= fail_after) {
                set_exit_reason(42);
                as_lifecycle()->set_failure_reason(error(42));
                as_lifecycle()->transition(LifecycleState::kFailed);
            }
        }};
    }
};

// ── ForwardingActor — forwards received messages to a fixed target
// ────────────

class ForwardingActor : public EventBasedActor, public LifecycleActor {
  public:
    ActorAddress target;
    int forwarded_count = 0;

    ForwardingActor(ActorContext* ctx, ActorSystem& sys, ActorAddress dest = {})
        : EventBasedActor(ctx, sys), target(dest) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    void set_target(ActorAddress dest) {
        target = dest;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            forwarded_count++;
            if (target.id != ActorId(0)) {
                context()->send(target, std::move(msg));
            }
        }};
    }
};

// ── FAIL macro for explicit test failure
// ──────────────────────────────────────

#define SYSTEM_TEST_FAIL(msg)                                                  \
    do {                                                                       \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);   \
        std::exit(1);                                                          \
    } while (0)

} // namespace hpactor::test