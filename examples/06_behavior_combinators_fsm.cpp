// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

// =============================================================================
// HPActor Example 06: Behavior Combinators, FSM DSL & StashBuffer
// =============================================================================
//
// Demonstrates the three new behavior composition features together:
//
//   1. Behavior Combinators (intercept, setup, compose, on_signal)
//      - Behavior::setup()         — deferred init with actor context
//      - Behavior::intercept()     — middleware wrapping (logging/metrics)
//      - Behavior::compose()       — layered handlers
//      - Behavior::on_signal()     — signal interception
//
//   2. FSM DSL (BehaviorFsmBuilder)
//      - Declarative state machine with state data
//      - Per-state typed message handlers via on_raw()
//      - Per-state idle timeouts (auto-cancel stale orders)
//      - on_transition() hooks for audit logging
//
//   3. StashBuffer
//      - Buffer messages during initialization
//      - Replay stashed messages once ready
//
// Scenario: Payment Order Processing
//
//   ┌──────────┐  ValidateCmd   ┌───────────┐  PayCmd   ┌──────┐
//   │ Pending  │──────────────→│ Validated  │─────────→│ Paid │
//   │ (300s    │               │            │          │      │
//   │  timeout)│               └───────────┘          └──┬───┘
//   │          │                                         │
//   │          │  CancelCmd (from any state)             │ ShipCmd
//   └────┬─────┘                                         │
//        └─────────────────────────────────────────→┌────┴──────┐
//                                                   │ Shipped   │
//                                                   └───────────┘
//                          CancelCmd                 ┌───────────┐
//                       ────────────────────────────→│ Cancelled │
//                                                   │ (terminal)│
//                                                   └───────────┘
// =============================================================================

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/fsm/behavior_fsm_builder.hpp>
#include <hpactor/actor/fsm/fsm_directive.hpp>
#include <hpactor/mailbox/stash_buffer.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Message type tags (application range 0x1000+)
// ---------------------------------------------------------------------------

static const hpactor::TypeTag ValidateTag{0x00001000};
static const hpactor::TypeTag PayTag{0x00001001};
static const hpactor::TypeTag ShipTag{0x00001002};
static const hpactor::TypeTag CancelTag{0x00001003};
static const hpactor::TypeTag StatusTag{0x00001004};
static const hpactor::TypeTag InitDoneTag{0x00001005};

// ---------------------------------------------------------------------------
// Payload helpers — encode/decode a string + int
// ---------------------------------------------------------------------------

struct OrderPayload {
    std::string order_id;
    int amount = 0;
};

static hpactor::StreamBuffer encode_order(const OrderPayload& p) {
    hpactor::StreamBuffer buf(p.order_id.size() + sizeof(int) + 1);
    std::memcpy(buf.data(), p.order_id.c_str(), p.order_id.size());
    *reinterpret_cast<int*>(buf.data() + p.order_id.size()) = p.amount;
    return buf;
}

static OrderPayload decode_order(const hpactor::StreamBuffer& payload) {
    OrderPayload p;
    p.order_id = std::string(reinterpret_cast<const char*>(payload.data()),
                             payload.size() - sizeof(int));
    std::memcpy(&p.amount, payload.data() + p.order_id.size(), sizeof(int));
    return p;
}

static hpactor::TypedMessage
make_msg(hpactor::TypeTag tag, const std::string& id, int amt = 0) {
    return hpactor::TypedMessage(tag, encode_order({id, amt}));
}

// =============================================================================
// OrderActor — processes orders through an FSM with combinators and stash
// =============================================================================

enum class OrderState { Pending, Validated, Paid, Shipped, Cancelled };

struct OrderData {
    std::string order_id;
    int amount = 0;
    int step_count = 0;
};

class OrderActor : public hpactor::EventBasedActor {
  public:
    OrderActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys), stash_(16) {}

    const char* state_name() const {
        switch (current_state_) {
            case OrderState::Pending:
                return "Pending";
            case OrderState::Validated:
                return "Validated";
            case OrderState::Paid:
                return "Paid";
            case OrderState::Shipped:
                return "Shipped";
            case OrderState::Cancelled:
                return "Cancelled";
        }
        return "?";
    }

  protected:
    hpactor::Behavior make_behavior() override {
        // ── Demo 1: Behavior::setup() — deferred initialization ──
        //
        // The setup factory is called on first message, giving us access
        // to the fully-wired actor context.  We use it to register an
        // init-done handler that replays stashed messages.
        return hpactor::Behavior::setup([this]() -> hpactor::Behavior {
            // ── Demo 2: StashBuffer — gate messages during init ──
            // Build the FSM but wrap it in an init gate: stash all
            // messages until we receive InitDoneTag.
            return hpactor::Behavior::intercept(
                build_fsm(), [this](hpactor::TypedMessage& msg,
                                    hpactor::Behavior::next_fn next) {
                    if (!initialized_) {
                        if (msg.type_id() == InitDoneTag) {
                            initialized_ = true;
                            std::cout << "  [OrderActor " << id().value()
                                      << "]: init complete — replaying "
                                      << stash_.size() << " stashed messages"
                                      << std::endl;
                            auto stashed = stash_.unstash_all();
                            for (auto& m : stashed)
                                next(m);
                            return;
                        }
                        // Stash all other messages until initialized.
                        stash_.try_stash(std::move(msg));
                        return;
                    }
                    next(msg);
                });
        });
    }

  private:
    // ── Demo 3: FSM DSL — declarative state machine ──
    hpactor::Behavior build_fsm() {
        using OD = hpactor::FsmDirective<OrderState, OrderData>;

        // ── Demo 4: on_transition() — audit logging on every state change ──
        auto builder =
            hpactor::BehaviorFsmBuilder<OrderState, OrderData>::start(
                OrderState::Pending, OrderData{"default", 0, 0})
                .on_transition(
                    [this](OrderState from, OrderState to, OrderData& data) {
                        current_state_ = to;
                        data.step_count++;
                        std::cout
                            << "  [OrderActor " << id().value()
                            << "]: transition " << state_name_for(from) << " → "
                            << state_name_for(to) << " (order=" << data.order_id
                            << ", step=" << data.step_count << ")" << std::endl;
                    });

        // ── Pending state ──
        builder.in_state(OrderState::Pending)
            .on_raw(ValidateTag,
                    [this](hpactor::TypedMessage& msg, OrderData& data) -> OD {
                        auto p = decode_order(msg.payload());
                        data.order_id = p.order_id;
                        data.amount = p.amount;
                        std::cout << "  [OrderActor " << id().value()
                                  << "]: Validating order " << data.order_id
                                  << " amount=" << data.amount << std::endl;
                        if (data.amount > 0)
                            return OD::go_to(OrderState::Validated);
                        std::cout << "    → invalid amount, staying in Pending"
                                  << std::endl;
                        return OD::stay();
                    })
            .on_raw(CancelTag,
                    [](hpactor::TypedMessage& /*msg*/, OrderData& /*data*/) -> OD {
                        return OD::go_to(OrderState::Cancelled);
                    })
            // Per-state timeout: auto-cancel after 300s idle in Pending
            .on_timeout(std::chrono::seconds(300), OrderState::Cancelled,
                        OrderData{});

        // ── Validated state ──
        builder.in_state(OrderState::Validated)
            .on_raw(PayTag,
                    [this](hpactor::TypedMessage& /*msg*/, OrderData& data) -> OD {
                        std::cout << "  [OrderActor " << id().value()
                                  << "]: Payment received for " << data.order_id
                                  << std::endl;
                        return OD::go_to(OrderState::Paid);
                    })
            .on_raw(CancelTag,
                    [](hpactor::TypedMessage& /*msg*/, OrderData& /*data*/) -> OD {
                        return OD::go_to(OrderState::Cancelled);
                    });

        // ── Paid state ──
        builder.in_state(OrderState::Paid)
            .on_raw(ShipTag,
                    [this](hpactor::TypedMessage& /*msg*/, OrderData& data) -> OD {
                        std::cout << "  [OrderActor " << id().value()
                                  << "]: Shipping order " << data.order_id
                                  << std::endl;
                        return OD::go_to(OrderState::Shipped);
                    });

        // ── Shipped state (terminal) ──
        builder.in_state(OrderState::Shipped);

        // ── Cancelled state (terminal) ──
        builder.in_state(OrderState::Cancelled);

        // ── Demo 5: Behavior::compose() — layer status handler on FSM ──
        auto fsm_bh = builder.build(this);

        auto status_handler =
            hpactor::Behavior::receive([this](hpactor::TypedMessage& msg) {
                if (msg.type_id() == StatusTag) {
                    auto p = decode_order(msg.payload());
                    std::cout << "  [OrderActor " << id().value()
                              << "]: STATUS — order=" << p.order_id
                              << " state=" << state_name()
                              << " amount=" << p.amount << std::endl;
                }
            });

        return hpactor::Behavior::compose(std::move(status_handler),
                                          std::move(fsm_bh));
    }

    static const char* state_name_for(OrderState s) {
        switch (s) {
            case OrderState::Pending:
                return "Pending";
            case OrderState::Validated:
                return "Validated";
            case OrderState::Paid:
                return "Paid";
            case OrderState::Shipped:
                return "Shipped";
            case OrderState::Cancelled:
                return "Cancelled";
        }
        return "?";
    }

    hpactor::StashBuffer stash_;
    bool initialized_ = false;
    OrderState current_state_ = OrderState::Pending;
};

// ── LoggingInterceptor — reusable middleware via Behavior::intercept() ──

class LoggingInterceptor {
  public:
    explicit LoggingInterceptor(const char* name) : name_(name) {}

    void
    operator()(hpactor::TypedMessage& msg, hpactor::Behavior::next_fn next) const {
        std::cout << "  [Log-" << name_
                  << "]: → tag=" << static_cast<uint32_t>(msg.type_id())
                  << " size=" << msg.payload().size() << std::endl;
        next(msg);
        std::cout << "  [Log-" << name_ << "]: ← done" << std::endl;
    }

  private:
    const char* name_;
};

// =============================================================================
// LoggedOrderActor — same FSM but with logging middleware
// =============================================================================

class LoggedOrderActor : public hpactor::EventBasedActor {
  public:
    LoggedOrderActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                     int actor_num)
        : hpactor::EventBasedActor(ctx, sys), actor_num_(actor_num) {}

  protected:
    hpactor::Behavior make_behavior() override {
        // ── Demo 6: Behavior::intercept() — logging middleware ──
        //
        // Wrap the entire FSM behavior with a logging interceptor.
        // Every message is logged before/after processing.
        LoggingInterceptor logger("order");
        return hpactor::Behavior::intercept(
            build_fsm(),
            [logger](hpactor::TypedMessage& msg,
                     hpactor::Behavior::next_fn next) { logger(msg, next); });
    }

  private:
    hpactor::Behavior build_fsm() {
        using OD = hpactor::FsmDirective<OrderState, OrderData>;

        return hpactor::BehaviorFsmBuilder<OrderState, OrderData>::start(
                   OrderState::Pending, OrderData{"order", 0, 0})
            .in_state(OrderState::Pending)
            .on_raw(ValidateTag,
                    [this](hpactor::TypedMessage& msg, OrderData& data) -> OD {
                        auto p = decode_order(msg.payload());
                        data.order_id = p.order_id;
                        data.amount = p.amount;
                        std::cout << "  [LoggedOrder-" << actor_num_
                                  << "]: validating " << data.order_id
                                  << std::endl;
                        if (data.amount > 0)
                            return OD::go_to(OrderState::Validated);
                        return OD::stay();
                    })
            .on_raw(CancelTag,
                    [](hpactor::TypedMessage&, OrderData&) -> OD {
                        return OD::go_to(OrderState::Cancelled);
                    })
            .on_timeout(std::chrono::seconds(300), OrderState::Cancelled,
                        OrderData{})
            .in_state(OrderState::Validated)
            .on_raw(PayTag,
                    [this](hpactor::TypedMessage&, OrderData& data) -> OD {
                        std::cout << "  [LoggedOrder-" << actor_num_
                                  << "]: paid " << data.order_id << std::endl;
                        return OD::go_to(OrderState::Paid);
                    })
            .in_state(OrderState::Paid)
            .on_raw(ShipTag,
                    [this](hpactor::TypedMessage&, OrderData& data) -> OD {
                        std::cout << "  [LoggedOrder-" << actor_num_
                                  << "]: shipped " << data.order_id << std::endl;
                        return OD::go_to(OrderState::Shipped);
                    })
            .on_transition([](OrderState from, OrderState to, OrderData&) {
                std::cout << "  [LoggedOrder]: " << static_cast<int>(from)
                          << " → " << static_cast<int>(to) << std::endl;
            })
            .build(this);
    }

    int actor_num_;
};

// =============================================================================
// Helper: deliver from main
// =============================================================================

static void
deliver(hpactor::ActorSystem& sys, hpactor::ActorId target,
        hpactor::TypeTag tag, const std::string& id = "", int amount = 0) {
    sys.deliver_local(target, make_msg(tag, id, amount));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 06: Behavior Combinators, FSM DSL & "
                 "StashBuffer ==="
              << std::endl;

    hpactor::Config config{.scheduler_threads = 2,
                           .max_queue_depth = 1024,
                           .cli = {},
                           .mailbox = {},
                           .dead_letters = {},
                           .tracing = {},
                           .process = {}};
    hpactor::ActorSystem system(config);

    // ═══════════════════════════════════════════════════════════════
    // Part A: OrderActor — FSM + setup() + StashBuffer + compose()
    // ═══════════════════════════════════════════════════════════════

    std::cout << "\n─── Part A: OrderActor (FSM + setup + stash + compose) "
                 "───"
              << std::endl;

    auto order_a = system.spawn<OrderActor>();
    std::cout << "Spawned OrderActor (id=" << order_a.id().value() << ")"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send messages BEFORE initialization — they get stashed.
    std::cout << "\n[Sending messages before init — should be stashed]" << std::endl;
    deliver(system, order_a.id(), ValidateTag, "order-A1", 150);
    deliver(system, order_a.id(), StatusTag, "order-A1", 150);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Now initialize — stashed messages are replayed.
    std::cout << "\n[Triggering initialization]" << std::endl;
    deliver(system, order_a.id(), InitDoneTag);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // After init, new messages flow through normally.
    std::cout << "\n[After init — messages flow through FSM]" << std::endl;
    deliver(system, order_a.id(), PayTag, "order-A1");
    deliver(system, order_a.id(), ShipTag, "order-A1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Demonstrate cancellation from any state.
    std::cout << "\n[New order — cancel from Pending]" << std::endl;
    deliver(system, order_a.id(), ValidateTag, "order-A2", 200);
    deliver(system, order_a.id(), CancelTag, "order-A2");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ═══════════════════════════════════════════════════════════════
    // Part B: LoggedOrderActor — FSM + intercept() middleware
    // ═══════════════════════════════════════════════════════════════

    std::cout << "\n─── Part B: LoggedOrderActor (FSM + intercept logging) ───"
              << std::endl;

    auto order_b = system.spawn<LoggedOrderActor>(1);
    std::cout << "Spawned LoggedOrderActor (id=" << order_b.id().value() << ")"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "\n[Processing order with logging middleware]" << std::endl;
    deliver(system, order_b.id(), ValidateTag, "order-B1", 500);
    deliver(system, order_b.id(), PayTag, "order-B1");
    deliver(system, order_b.id(), ShipTag, "order-B1");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ═══════════════════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════════════════

    std::cout << "\n─── API Reference ───" << std::endl;
    std::cout << "  // Combinators" << std::endl;
    std::cout << "  Behavior::setup(factory)        — deferred init" << std::endl;
    std::cout << "  Behavior::intercept(inner, fn)  — middleware wrapping"
              << std::endl;
    std::cout << "  Behavior::compose(a, b)         — layered handlers" << std::endl;
    std::cout << "  Behavior::on_signal(tag, h, in) — signal interception"
              << std::endl;
    std::cout << "  Behavior::receive(handler)      — simple handler" << std::endl;
    std::cout << std::endl;
    std::cout << "  // FSM DSL" << std::endl;
    std::cout << "  BehaviorFsmBuilder<S,D>::start(state, data)" << std::endl;
    std::cout << "    .in_state(S).on_raw(tag, fn)          — per-state handler"
              << std::endl;
    std::cout << "    .in_state(S).on_timeout(dur, S, D)     — idle timeout"
              << std::endl;
    std::cout << "    .on_transition(fn)                     — transition hook"
              << std::endl;
    std::cout << "    .build(this)                            — produce Behavior"
              << std::endl;
    std::cout << std::endl;
    std::cout << "  // FsmDirective return values from handlers" << std::endl;
    std::cout << "  FsmDirective::stay() / stay(data)   — keep state" << std::endl;
    std::cout << "  FsmDirective::go_to(S) / go_to(S,D) — transition" << std::endl;
    std::cout << "  FsmDirective::stop()                — terminate" << std::endl;
    std::cout << std::endl;
    std::cout << "  // StashBuffer" << std::endl;
    std::cout << "  StashBuffer buf(capacity);" << std::endl;
    std::cout << "  buf.try_stash(msg)       — buffer or reject if full"
              << std::endl;
    std::cout << "  buf.unstash_all()         — drain all stashed messages"
              << std::endl;
    std::cout << "  buf.unstash_one()         — pop oldest (FIFO)" << std::endl;
    std::cout << "  buf.clear()               — drop all" << std::endl;

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
