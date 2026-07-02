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

#include <hpactor/mailbox/disruptor_message_envelope.hpp>
#include <hpactor/mailbox/mailbox_kind.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>

namespace hpactor::mailbox {
namespace {

// ── Disruptor-message concept
// ─────────────────────────────────────────────────

struct Quote {
    uint64_t instrument;
    int64_t price;
};

struct CancelQuote {
    uint64_t instrument;
};

struct NonTrivial {
    std::string value;
};

struct NonStandardLayout {
  private:
    [[maybe_unused]] int a;

  public:
    int b;
};

// Compile-time concept checks
static_assert(DisruptorMessage<Quote>);
static_assert(DisruptorMessage<CancelQuote>);
static_assert(DisruptorMessage<uint64_t>);
static_assert(DisruptorMessage<int32_t>);
static_assert(!DisruptorMessage<NonTrivial>);
static_assert(!DisruptorMessage<NonStandardLayout>);
static_assert(!DisruptorMessage<std::string>);

// ── Capacity validation ───────────────────────────────────────────────────

static_assert(valid_disruptor_ring_capacity(2));
static_assert(valid_disruptor_ring_capacity(4));
static_assert(valid_disruptor_ring_capacity(1024));
static_assert(valid_disruptor_ring_capacity(1u << 20));
static_assert(!valid_disruptor_ring_capacity(0));
static_assert(!valid_disruptor_ring_capacity(1));
static_assert(!valid_disruptor_ring_capacity(3));
static_assert(!valid_disruptor_ring_capacity(5));
static_assert(!valid_disruptor_ring_capacity(100));
static_assert(!valid_disruptor_ring_capacity((1u << 20) + 1));

// ── MailboxKind ───────────────────────────────────────────────────────────

static_assert(static_cast<uint8_t>(MailboxKind::VariableMpsc) == 0);
static_assert(static_cast<uint8_t>(MailboxKind::Disruptor) == 1);

// ── DisruptorSendOptions
// ──────────────────────────────────────────────────────

TEST(DisruptorSendOptionsTest, DefaultOptions) {
    DisruptorSendOptions opts;
    EXPECT_EQ(opts.deadline_ns, INT64_MAX);
    EXPECT_EQ(opts.message_id, 0u);
    EXPECT_EQ(opts.flags, 0u);
}

TEST(DisruptorSendOptionsTest, ExplicitOptions) {
    DisruptorSendOptions opts;
    opts.deadline_ns = 5000000;
    opts.message_id = 42;
    opts.flags = 0x01;
    EXPECT_EQ(opts.deadline_ns, 5000000);
    EXPECT_EQ(opts.message_id, 42u);
    EXPECT_EQ(opts.flags, 0x01u);
}

// ── DisruptorEnvelopeMeta
// ─────────────────────────────────────────────────────

TEST(DisruptorEnvelopeMetaTest, DefaultMeta) {
    DisruptorEnvelopeMeta meta;
    EXPECT_EQ(meta.deadline_ns, INT64_MAX);
    EXPECT_EQ(meta.message_id, 0u);
    EXPECT_EQ(meta.enqueue_sequence, 0u);
    EXPECT_EQ(meta.flags, 0u);
    EXPECT_FALSE(meta.has_trace);
}

TEST(DisruptorEnvelopeMetaTest, SetFields) {
    DisruptorEnvelopeMeta meta;
    meta.message_id = 99;
    meta.deadline_ns = 1000;
    meta.enqueue_sequence = 7;
    meta.flags = 0x02;
    meta.has_trace = true;
    EXPECT_EQ(meta.message_id, 99u);
    EXPECT_EQ(meta.deadline_ns, 1000);
    EXPECT_EQ(meta.enqueue_sequence, 7u);
    EXPECT_EQ(meta.flags, 0x02u);
    EXPECT_TRUE(meta.has_trace);
}

// ── DisruptorMessageEnvelope
// ──────────────────────────────────────────────────

using QuoteEnvelope = DisruptorMessageEnvelope<Quote>;

TEST(DisruptorMessageEnvelopeTest, DefaultConstruction) {
    QuoteEnvelope env;
    // Default-constructed variant holds the first alternative.
    EXPECT_TRUE(std::holds_alternative<Quote>(env.message));
    EXPECT_EQ(env.meta.message_id, 0u);
}

TEST(DisruptorMessageEnvelopeTest, AssignMessageAndMetadata) {
    QuoteEnvelope env;
    env.message = Quote{7, 10125};
    env.meta.message_id = 44;
    env.meta.deadline_ns = 9000;

    ASSERT_TRUE(std::holds_alternative<Quote>(env.message));
    EXPECT_EQ(std::get<Quote>(env.message).instrument, 7u);
    EXPECT_EQ(std::get<Quote>(env.message).price, 10125);
    EXPECT_EQ(env.meta.message_id, 44u);
    EXPECT_EQ(env.meta.deadline_ns, 9000);
}

using MultiEnvelope = DisruptorMessageEnvelope<Quote, CancelQuote>;

TEST(DisruptorMessageEnvelopeTest, MultipleAlternatives) {
    MultiEnvelope env;
    // Default constructs the first alternative.
    EXPECT_TRUE(std::holds_alternative<Quote>(env.message));

    // Switch to CancelQuote.
    env.message = CancelQuote{42};
    ASSERT_TRUE(std::holds_alternative<CancelQuote>(env.message));
    EXPECT_EQ(std::get<CancelQuote>(env.message).instrument, 42u);

    // Switch back to Quote.
    env.message = Quote{1, 100};
    ASSERT_TRUE(std::holds_alternative<Quote>(env.message));
    EXPECT_EQ(std::get<Quote>(env.message).instrument, 1u);
    EXPECT_EQ(std::get<Quote>(env.message).price, 100);
}

TEST(DisruptorMessageEnvelopeTest, VariantIndexOfMatchesOrder) {
    MultiEnvelope env;
    env.message = Quote{};
    EXPECT_EQ(env.message.index(), 0u);
    env.message = CancelQuote{};
    EXPECT_EQ(env.message.index(), 1u);
}

} // namespace
} // namespace hpactor::mailbox
