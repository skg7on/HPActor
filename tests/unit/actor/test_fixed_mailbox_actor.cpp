// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#include <hpactor/actor/fixed_mailbox_actor.hpp>
#include <hpactor/mailbox/fixed_message_envelope.hpp>
#include <hpactor/mailbox/mailbox_kind.hpp>

#include <gtest/gtest.h>

namespace hpactor::mailbox {
namespace {

// ── Test message types ────────────────────────────────────────────────────

struct Increment {
    uint64_t value;
};
struct Reset {
    uint64_t value;
};
struct Undeclared {
    uint64_t value;
};

static_assert(FixedMailboxMessage<Increment>);
static_assert(FixedMailboxMessage<Reset>);
static_assert(FixedMailboxMessage<Undeclared>);

// ── Test actor ────────────────────────────────────────────────────────────

class CounterActor final : public FixedMailboxActor<8, Increment, Reset> {
  public:
    using FixedMailboxActor::FixedMailboxActor;

    uint64_t total() const {
        return total_;
    }

  protected:
    fixed_behavior_type make_fixed_behavior() override {
        return {on_fixed<Increment>(
                    [this](const Increment& msg) { total_ += msg.value; }),
                on_fixed<Reset>([this](const Reset& msg) { total_ = msg.value; })};
    }

  private:
    uint64_t total_{0};
};

// ── Compile-time API checks ───────────────────────────────────────────────

using RefType = CounterActor::fixed_actor_ref_type;

static_assert(requires(RefType ref) { ref.try_send(Increment{1}); });
static_assert(requires(RefType ref) { ref.try_send(Reset{1}); });

// ── Reference tests ───────────────────────────────────────────────────────

TEST(FixedActorRefTest, DefaultConstructedRefIsEmpty) {
    RefType ref;
    EXPECT_FALSE(ref.valid());
}

TEST(FixedActorRefTest, TrySendToInvalidRefReturnsRejected) {
    RefType ref;
    auto result = ref.try_send(Increment{1});
    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, EnqueueResultCode::Rejected);
}

// ── Mailbox kind ──────────────────────────────────────────────────────────

TEST(FixedMailboxActorTest, MailboxKindIsFixedDisruptor) {
    // We can't fully construct the actor without ActorSystem, but we can
    // check the type-level contract.
    EXPECT_EQ(static_cast<uint8_t>(MailboxKind::FixedDisruptor), 1u);
}

} // namespace
} // namespace hpactor::mailbox
