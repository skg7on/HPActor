// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

namespace {

// User tags used for testing (must be >= 0x1000 to avoid system-message
// treatment, and must have distinct low bytes to avoid bitset collisions).
inline constexpr TypeTag kFastTagA{0x1001};  // low byte 0x01
inline constexpr TypeTag kFastTagB{0x1002};  // low byte 0x02
inline constexpr TypeTag kNormalTag{0x1003}; // low byte 0x03 — NOT
                                             // fast-registered

// Actor that counts messages by tag for fast-tag verification.
class CountActor : public EventBasedActor {
  public:
    using EventBasedActor::EventBasedActor;

    void init_behavior() {
        become(make_behavior());
    }

    uint32_t tag_a_count = 0;
    uint32_t tag_b_count = 0;
    uint32_t other_count = 0;
    bool handler_called = false;

    void do_add_fast_tag(TypeTag tag) {
        add_fast_tag(tag);
    }
    bool has_fast_tag(TypeTag tag) const {
        return is_fast_tag(tag);
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            handler_called = true;
            if (msg.type_id() == kFastTagA)
                tag_a_count++;
            else if (msg.type_id() == kFastTagB)
                tag_b_count++;
            else
                other_count++;
        }};
    }
};

} // namespace

class FastTagReceiveTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
    }

    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }

    std::unique_ptr<ActorSystem> system_;
};

// ── Fast-tag message reaches the handler ──────────────────────────
TEST_F(FastTagReceiveTest, FastTagMessageDispatched) {
    auto actor = system_->spawn<CountActor>();
    auto* raw = static_cast<CountActor*>(actor.get().get());
    raw->init_behavior();
    raw->do_add_fast_tag(kFastTagA);

    TypedMessage msg(kFastTagA, StreamBuffer{1});
    raw->receive(msg);

    EXPECT_TRUE(raw->handler_called);
    EXPECT_EQ(raw->tag_a_count, 1u);
}

// ── Non-fast-tag message reaches handler via normal pipeline ───────
TEST_F(FastTagReceiveTest, NonFastTagMessageDispatched) {
    auto actor = system_->spawn<CountActor>();
    auto* raw = static_cast<CountActor*>(actor.get().get());
    raw->init_behavior();
    raw->do_add_fast_tag(kFastTagA); // only A is fast

    TypedMessage msg(kNormalTag, StreamBuffer{1});
    raw->receive(msg);

    EXPECT_TRUE(raw->handler_called);
    EXPECT_EQ(raw->tag_a_count, 0u);
    EXPECT_EQ(raw->other_count, 1u);
}

// ── Multiple fast tags registered ─────────────────────────────────
TEST_F(FastTagReceiveTest, MultipleFastTags) {
    auto actor = system_->spawn<CountActor>();
    auto* raw = static_cast<CountActor*>(actor.get().get());
    raw->init_behavior();
    raw->do_add_fast_tag(kFastTagA);
    raw->do_add_fast_tag(kFastTagB);

    EXPECT_TRUE(raw->has_fast_tag(kFastTagA));
    EXPECT_TRUE(raw->has_fast_tag(kFastTagB));
    EXPECT_FALSE(raw->has_fast_tag(kNormalTag));

    // First fast tag
    TypedMessage msg1(kFastTagA, StreamBuffer{1});
    raw->receive(msg1);
    EXPECT_EQ(raw->tag_a_count, 1u);

    // Second fast tag
    TypedMessage msg2(kFastTagB, StreamBuffer{1});
    raw->receive(msg2);
    EXPECT_EQ(raw->tag_b_count, 1u);
}

// ── No fast tags = all messages go through normal pipeline ─────────
TEST_F(FastTagReceiveTest, NoFastTagsAllNormal) {
    auto actor = system_->spawn<CountActor>();
    auto* raw = static_cast<CountActor*>(actor.get().get());
    raw->init_behavior();

    // kNormalTag is NOT registered as fast — goes through full pipeline.
    TypedMessage msg(kNormalTag, StreamBuffer{1});
    raw->receive(msg);

    EXPECT_TRUE(raw->handler_called);
    EXPECT_EQ(raw->other_count, 1u);
}

// ── System message still processes through normal path ─────────────
TEST_F(FastTagReceiveTest, SystemMessageStillDispatched) {
    auto actor = system_->spawn<CountActor>();
    auto* raw = static_cast<CountActor*>(actor.get().get());
    raw->init_behavior();

    TypedMessage msg(TypeTag::LinkMsg, StreamBuffer{1});
    msg.set_sender_address(hpactor::ActorAddress{});
    raw->receive(msg);

    // LinkMsg is consumed by dispatch_system_message() — the behavior
    // handler should NOT be called.
    EXPECT_FALSE(raw->handler_called);
}

// ── Fast tag skips pipeline overhead: both fast and normal dispatch correctly
// ──
TEST_F(FastTagReceiveTest, FastAndNormalTagsBothReachHandler) {
    auto actor = system_->spawn<CountActor>();
    auto* raw = static_cast<CountActor*>(actor.get().get());
    raw->init_behavior();
    raw->do_add_fast_tag(kFastTagA);

    // Fast tag: should reach handler.
    {
        TypedMessage msg(kFastTagA, StreamBuffer{1});
        raw->handler_called = false;
        raw->receive(msg);
        EXPECT_TRUE(raw->handler_called);
        EXPECT_EQ(raw->tag_a_count, 1u);
    }
    // Normal tag: should also reach handler via full pipeline.
    {
        TypedMessage msg(kNormalTag, StreamBuffer{1});
        raw->handler_called = false;
        raw->receive(msg);
        EXPECT_TRUE(raw->handler_called);
        EXPECT_EQ(raw->other_count, 1u);
    }
}

// ── System message ignored by fast-tag check ──────────────────────
TEST_F(FastTagReceiveTest, FastTagDoesNotAffectSystemMessage) {
    auto actor = system_->spawn<CountActor>();
    auto* raw = static_cast<CountActor*>(actor.get().get());
    raw->init_behavior();
    // System messages (tag < 0x1000) are never fast-pathed regardless
    // of the bitset state.
    raw->do_add_fast_tag(TypeTag::LinkMsg); // would set bit, but ignored

    TypedMessage msg(TypeTag::LinkMsg, StreamBuffer{1});
    msg.set_sender_address(hpactor::ActorAddress{});
    raw->receive(msg);

    EXPECT_FALSE(raw->handler_called) << "system message must go through "
                                         "dispatch_system_message, not fast path";
}
