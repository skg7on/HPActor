// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/msg/failure_envelope.hpp>
#include <hpactor/msg/message_id.hpp>
#include <hpactor/msg/proto_type_registry.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <scheduler_test_driver.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace hpactor {
namespace {

// helper: append string data to a StreamBuffer
void append_str(StreamBuffer& buf, const std::string& s) {
    buf.append(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ============================================================================
// TypedMessage construction with various TypeTags
// ============================================================================

TEST(MessageIntegrationTest, TypedMessageDefaultConstruction) {
    TypedMessage msg;
    EXPECT_EQ(msg.type_id(), TypeTag::Invalid);
    EXPECT_TRUE(msg.payload().empty());
    EXPECT_EQ(msg.parsed(), nullptr);
    EXPECT_FALSE(msg.has_trace_context());
    EXPECT_EQ(msg.deadline_ns(), INT64_MAX);
    EXPECT_EQ(msg.ask_message_id(), 0u);
}

TEST(MessageIntegrationTest, TypedMessageConstructionWithSystemTags) {
    // Test with DownMsg tag
    StreamBuffer payload;
    append_str(payload, "test-down-msg");
    TypedMessage msg(TypeTag::DownMsg, payload);
    EXPECT_EQ(msg.type_id(), TypeTag::DownMsg);
    EXPECT_FALSE(msg.payload().empty());
    EXPECT_EQ(msg.payload().size(), 13u);

    // Test with ExitMsg tag
    TypedMessage msg2(TypeTag::ExitMsg, StreamBuffer{});
    EXPECT_EQ(msg2.type_id(), TypeTag::ExitMsg);

    // Test with ErrorMsg tag
    TypedMessage msg3(TypeTag::ErrorMsg, StreamBuffer{});
    EXPECT_EQ(msg3.type_id(), TypeTag::ErrorMsg);
}

TEST(MessageIntegrationTest, TypedMessageConstructionWithSpawnTags) {
    StreamBuffer payload;
    append_str(payload, "spawn-data");

    TypedMessage req(TypeTag::SpawnRequestTag, payload);
    EXPECT_EQ(req.type_id(), TypeTag::SpawnRequestTag);
    EXPECT_EQ(req.payload().size(), 10u);

    TypedMessage resp(TypeTag::SpawnResponseTag, payload);
    EXPECT_EQ(resp.type_id(), TypeTag::SpawnResponseTag);
}

TEST(MessageIntegrationTest, TypedMessageConstructionWithCliTags) {
    StreamBuffer buf;
    append_str(buf, "cli-data");

    TypedMessage msg(TypeTag::InspectStateRequestTag, buf);
    EXPECT_EQ(msg.type_id(), TypeTag::InspectStateRequestTag);

    TypedMessage msg2(TypeTag::KillRequestTag, buf);
    EXPECT_EQ(msg2.type_id(), TypeTag::KillRequestTag);
}

// ============================================================================
// Message serialization roundtrip
// ============================================================================

TEST(MessageIntegrationTest, TypedMessageSerializationRoundtrip) {
    ProtoTypeRegistry registry;
    registry.register_system_types();

    // Construct a protobuf message (use PbLocalActorAddress from common.pb.h)
    ::hpactor::PbLocalActorAddress local;
    local.set_actor_id(42);
    local.set_actor_type(10);
    local.set_incarnation(1);

    // Serialize to StreamBuffer
    std::string serialized = local.SerializeAsString();
    StreamBuffer buffer;
    buffer.append(reinterpret_cast<const uint8_t*>(serialized.data()),
                  serialized.size());

    // Create a TypedMessage with the serialized data
    TypedMessage msg(TypeTag::User, buffer);
    EXPECT_EQ(msg.type_id(), TypeTag::User);
    EXPECT_EQ(msg.payload().size(), serialized.size());

    // Lazy deserialize via as<>()
    auto parsed = msg.as<::hpactor::PbLocalActorAddress>();
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->actor_id(), 42u);
    EXPECT_EQ(parsed->actor_type(), 10u);
    EXPECT_EQ(parsed->incarnation(), 1u);

    // Second call returns cached instance
    auto parsed2 = msg.as<::hpactor::PbLocalActorAddress>();
    EXPECT_EQ(parsed2, parsed);
}

TEST(MessageIntegrationTest, TypedMessageNullPayloadLazyParse) {
    TypedMessage msg;
    auto parsed = msg.as<::hpactor::PbLocalActorAddress>();
    EXPECT_EQ(parsed, nullptr);
}

// ============================================================================
// Delivery mode configuration
// ============================================================================

TEST(MessageIntegrationTest, DeliveryModeToStr) {
    using namespace hpactor::mailbox;
    EXPECT_STREQ(to_string(DeliveryMode::BestEffort), "best_effort");
    EXPECT_STREQ(to_string(DeliveryMode::ObservableBestEffort),
                 "observable_best_effort");
    EXPECT_STREQ(to_string(DeliveryMode::AtLeastOnce), "at_least_once");
    EXPECT_STREQ(to_string(DeliveryMode::DurableAtLeastOnce),
                 "durable_at_least_once");
}

TEST(MessageIntegrationTest, DeliveryModeIsTracked) {
    using namespace hpactor::mailbox;
    EXPECT_FALSE(is_tracked_delivery(DeliveryMode::BestEffort));
    EXPECT_FALSE(is_tracked_delivery(DeliveryMode::ObservableBestEffort));
    EXPECT_TRUE(is_tracked_delivery(DeliveryMode::AtLeastOnce));
    EXPECT_TRUE(is_tracked_delivery(DeliveryMode::DurableAtLeastOnce));
}

// ============================================================================
// DedupCache with various message patterns
// ============================================================================

TEST(MessageIntegrationTest, DedupCacheBasicInsertAndDuplicate) {
    adt::DedupCache::Config config;
    config.max_entries = 100;
    config.ttl_ns = 60'000'000'000ULL; // 60 seconds
    adt::DedupCache cache(config);

    Ipv4Endpoint ep(0x7F000001, 0); // 127.0.0.1
    ActorId actor(1);
    MessageId msg_id(100);

    // First call should not be duplicate
    EXPECT_FALSE(cache.is_duplicate(ep, actor, msg_id));
    EXPECT_EQ(cache.insertions(), 1u);
    EXPECT_EQ(cache.duplicate_hits(), 0u);

    // Same (source, actor, message_id) should be duplicate
    EXPECT_TRUE(cache.is_duplicate(ep, actor, msg_id));
    EXPECT_EQ(cache.duplicate_hits(), 1u);
    EXPECT_EQ(cache.insertions(), 1u); // No new insertion

    // Different message_id should not be duplicate
    MessageId msg_id2(200);
    EXPECT_FALSE(cache.is_duplicate(ep, actor, msg_id2));
    EXPECT_EQ(cache.insertions(), 2u);
}

TEST(MessageIntegrationTest, DedupCacheDifferentActorOrSource) {
    adt::DedupCache::Config config;
    config.max_entries = 100;
    adt::DedupCache cache(config);

    Ipv4Endpoint ep1(0x7F000001, 0);
    Ipv4Endpoint ep2(0x7F000002, 0);
    ActorId actor1(1);
    ActorId actor2(2);
    MessageId msg_id(100);

    // Different source should not be duplicate
    EXPECT_FALSE(cache.is_duplicate(ep1, actor1, msg_id));
    EXPECT_FALSE(cache.is_duplicate(ep2, actor1, msg_id));
    EXPECT_FALSE(cache.is_duplicate(ep1, actor2, msg_id));

    // Same tuple should be duplicate
    EXPECT_TRUE(cache.is_duplicate(ep1, actor1, msg_id));
    EXPECT_EQ(cache.duplicate_hits(), 1u);
}

TEST(MessageIntegrationTest, DedupCacheSizeTracking) {
    adt::DedupCache::Config config;
    config.max_entries = 50;
    adt::DedupCache cache(config);

    Ipv4Endpoint ep(0x7F000001, 0);
    ActorId actor(1);

    // Insert 10 unique messages
    for (uint64_t i = 1; i <= 10; ++i) {
        MessageId msg_id(i);
        EXPECT_FALSE(cache.is_duplicate(ep, actor, msg_id));
    }
    EXPECT_EQ(cache.insertions(), 10u);
    EXPECT_GE(cache.size(), 10u);
    EXPECT_EQ(cache.duplicate_hits(), 0u);

    // Re-check same messages - all should be duplicates
    for (uint64_t i = 1; i <= 10; ++i) {
        MessageId msg_id(i);
        EXPECT_TRUE(cache.is_duplicate(ep, actor, msg_id));
    }
    EXPECT_EQ(cache.duplicate_hits(), 10u);
    EXPECT_EQ(cache.insertions(), 10u); // No new insertions
}

// ============================================================================
// Failure envelope construction
// ============================================================================

TEST(MessageIntegrationTest, FailureEnvelopeDefaultConstruction) {
    FailureEnvelope env;
    EXPECT_EQ(env.reason, FailureReason::Unknown);
    EXPECT_EQ(env.actor_id, ActorId{});
    EXPECT_FALSE(env.retryable);
    EXPECT_EQ(env.timestamp_ns, 0u);
    EXPECT_EQ(env.source, FailureSource::ActorRuntime);
    EXPECT_EQ(env.detail_len, 0u);
}

TEST(MessageIntegrationTest, MakeFailureEnvelopeAllFields) {
    ActorId actor_id(42);
    ActorAddress sender;
    ActorAddress receiver;
    MessageId msg_id(100);
    TraceContext trace;

    auto env = make_failure_envelope(
        FailureReason::MailboxFull, actor_id, sender, receiver, msg_id, trace,
        FailureSource::Mailbox, "mailbox at capacity");
    EXPECT_EQ(env.reason, FailureReason::MailboxFull);
    EXPECT_EQ(env.actor_id, actor_id);
    EXPECT_EQ(env.message_id, msg_id);
    EXPECT_TRUE(env.retryable); // MailboxFull is retryable
    EXPECT_GT(env.timestamp_ns, 0u);
    EXPECT_EQ(env.source, FailureSource::Mailbox);
    EXPECT_GT(env.detail_len, 0u);
    EXPECT_EQ(env.detail_view(), "mailbox at capacity");
}

TEST(MessageIntegrationTest, MakeFailureEnvelopeNonRetryable) {
    auto env = make_failure_envelope(
        FailureReason::ActorDead, ActorId(1), ActorAddress{}, ActorAddress{},
        MessageId(1), TraceContext{}, FailureSource::ActorRuntime, "");
    EXPECT_EQ(env.reason, FailureReason::ActorDead);
    EXPECT_FALSE(env.retryable);
}

TEST(MessageIntegrationTest, FailureEnvelopeDetailTruncation) {
    FailureEnvelope env;
    // Set a detail string longer than 255 bytes
    std::string long_detail(500, 'x');
    env.set_detail(long_detail);
    EXPECT_EQ(env.detail_len, 255u);
    EXPECT_EQ(env.detail_view(), std::string_view(long_detail.data(), 255));
}

// ============================================================================
// Message routing edge cases
// ============================================================================

TEST(MessageIntegrationTest, MessageIdGenerationIsMonotonic) {
    MessageId id1 = generate_message_id();
    MessageId id2 = generate_message_id();
    MessageId id3 = generate_message_id();
    EXPECT_LT(id1.value(), id2.value());
    EXPECT_LT(id2.value(), id3.value());
}

TEST(MessageIntegrationTest, TypeTagHasDistinctErrorCode) {
    // ErrorMsg is a distinct TypeTag
    EXPECT_EQ(static_cast<uint32_t>(TypeTag::ErrorMsg), 0x12u);
    EXPECT_NE(TypeTag::ErrorMsg, TypeTag::DownMsg);
    EXPECT_NE(TypeTag::ErrorMsg, TypeTag::ExitMsg);
}

TEST(MessageIntegrationTest, TypeTagSystemRangeValues) {
    // Verify system range boundaries
    auto down_val = static_cast<uint32_t>(TypeTag::DownMsg);
    EXPECT_GE(down_val, 0x01u);
    EXPECT_LE(down_val, 0xFFu);

    auto err_val = static_cast<uint32_t>(TypeTag::ErrorMsg);
    EXPECT_GE(err_val, 0x01u);
    EXPECT_LE(err_val, 0xFFu);

    // User tag starts at 0x1000
    EXPECT_GE(static_cast<uint32_t>(TypeTag::User), 0x1000u);

    // BackpressureSignalTag is in system range
    auto bp = static_cast<uint32_t>(TypeTag::BackpressureSignalTag);
    EXPECT_GE(bp, 0x01u);
    EXPECT_LE(bp, 0xFFu);
}

// ============================================================================
// Error message types
// ============================================================================

TEST(MessageIntegrationTest, ErrorCodesMapToFailureReason) {
    // Verify individual error code mappings
    error e1(errors::unknown, "test");
    EXPECT_EQ(e1.failure_reason(), FailureReason::Unknown);

    error e2(errors::actor_down, "test");
    EXPECT_EQ(e2.failure_reason(), FailureReason::ActorDead);

    error e3(errors::actor_not_found, "test");
    EXPECT_EQ(e3.failure_reason(), FailureReason::NoRoute);

    error e4(errors::mailbox_full, "test");
    EXPECT_EQ(e4.failure_reason(), FailureReason::MailboxFull);

    error e5(errors::timeout, "test");
    EXPECT_EQ(e5.failure_reason(), FailureReason::Timeout);

    error e6(errors::invalid_argument, "test");
    EXPECT_EQ(e6.failure_reason(), FailureReason::RejectedByPolicy);

    error e7(errors::cancelled, "test");
    EXPECT_EQ(e7.failure_reason(), FailureReason::Dropped);

    // Unmapped code returns Unknown
    error e8(99999, "test");
    EXPECT_EQ(e8.failure_reason(), FailureReason::Unknown);
}

TEST(MessageIntegrationTest, ErrorConstructionAndOk) {
    error ok_err;
    EXPECT_TRUE(ok_err.ok());
    EXPECT_EQ(ok_err.code(), 0u);

    error fail_err(42, "something went wrong");
    EXPECT_FALSE(fail_err.ok());
    EXPECT_EQ(fail_err.code(), 42u);
    EXPECT_EQ(fail_err.message(), "something went wrong");
}

TEST(MessageIntegrationTest, ResultHasValueAndError) {
    auto r1 = result<int>::make(42);
    EXPECT_TRUE(r1.ok());
    EXPECT_TRUE(r1.has_value());
    EXPECT_FALSE(r1.is_error());
    EXPECT_EQ(r1.value(), 42);

    auto r2 = result<int>::make(error(errors::timeout, "timed out"));
    EXPECT_FALSE(r2.ok());
    EXPECT_FALSE(r2.has_value());
    EXPECT_TRUE(r2.is_error());
    EXPECT_EQ(r2.error().code(), errors::timeout);
}

TEST(MessageIntegrationTest, ResultVoid) {
    auto r1 = result<void>::make();
    EXPECT_TRUE(r1.ok());
    EXPECT_FALSE(r1.is_error());

    auto r2 = result<void>::make(error(errors::timeout, "timeout"));
    EXPECT_FALSE(r2.ok());
    EXPECT_TRUE(r2.is_error());
    EXPECT_EQ(r2.error().code(), errors::timeout);
}

// ============================================================================
// MessageVariant dispatch
// ============================================================================

TEST(MessageIntegrationTest, TypedMessageMoveSemantics) {
    StreamBuffer payload;
    append_str(payload, "move-test");
    TypedMessage src(TypeTag::User, payload);

    EXPECT_EQ(src.type_id(), TypeTag::User);
    EXPECT_EQ(src.payload().size(), 9u);

    // Move-construct
    TypedMessage dst(std::move(src));
    EXPECT_EQ(dst.type_id(), TypeTag::User);
    EXPECT_EQ(dst.payload().size(), 9u);

    // Move-assign
    StreamBuffer payload2;
    append_str(payload2, "assign-test");
    TypedMessage src2(TypeTag::DownMsg, payload2);
    dst = std::move(src2);
    EXPECT_EQ(dst.type_id(), TypeTag::DownMsg);
    EXPECT_EQ(dst.payload().size(), 11u);
}

TEST(MessageIntegrationTest, TypedMessageTraceContext) {
    TypedMessage msg;
    EXPECT_FALSE(msg.has_trace_context());

    TraceContext ctx;
    ctx.trace_id.bytes = {1, 2,  3,  4,  5,  6,  7,  8,
                          9, 10, 11, 12, 13, 14, 15, 16};
    ctx.span_id.bytes = {1, 2, 3, 4, 5, 6, 7, 8};
    ctx.flags.set_sampled(true);

    msg.set_trace_context(ctx);
    EXPECT_TRUE(msg.has_trace_context());
    EXPECT_TRUE(msg.trace_context().flags.sampled());

    msg.clear_trace_context();
    EXPECT_FALSE(msg.has_trace_context());
}

TEST(MessageIntegrationTest, TypedMessageDeadlineAndAskId) {
    TypedMessage msg;
    EXPECT_EQ(msg.deadline_ns(), INT64_MAX);
    EXPECT_EQ(msg.ask_message_id(), 0u);

    msg.set_deadline_ns(1000000);
    EXPECT_EQ(msg.deadline_ns(), 1000000);

    msg.set_ask_message_id(42);
    EXPECT_EQ(msg.ask_message_id(), 42u);
}

TEST(MessageIntegrationTest, TypedMessageSenderAddress) {
    TypedMessage msg;
    ActorAddress addr;
    msg.set_sender_address(addr);
    // Verify the sender address was stored
    EXPECT_EQ(msg.sender_address(), addr);
}

TEST(MessageIntegrationTest, TypeTagMakeSubsystemTag) {
    // make_subsystem_tag constructs tags in the 0x80-0xFF range at compile time
    constexpr TypeTag tag = make_subsystem_tag(0x80);
    auto val = static_cast<uint32_t>(tag);
    EXPECT_GE(val, 0x80u);
    EXPECT_LE(val, 0xFFu);
}

// ============================================================================
// Delivery metadata preservation
// ============================================================================

TEST(MessageIntegrationTest, TypedMessageDeliveryMetadataSurvivesMove) {
    TypedMessage original(TypeTag::User, StreamBuffer{1, 2, 3});
    original.set_delivery_priority(3);
    original.set_delivery_flags(0xA5A50001u);

    TypedMessage moved(std::move(original));
    EXPECT_EQ(moved.delivery_priority(), 3u);
    EXPECT_EQ(moved.delivery_flags(), 0xA5A50001u);

    TypedMessage assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.delivery_priority(), 3u);
    EXPECT_EQ(assigned.delivery_flags(), 0xA5A50001u);
}

namespace {
struct DeliveryMetadataObservation {
    std::atomic<uint8_t> priority{0};
    std::atomic<uint32_t> flags{0};
};

class DeliveryMetadataProbe final : public EventBasedActor {
  public:
    DeliveryMetadataProbe(ActorContext* ctx, ActorSystem& sys,
                          DeliveryMetadataObservation& observation)
        : EventBasedActor(ctx, sys), observation_(observation) {
        become(make_behavior());
    }

  protected:
    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            observation_.priority.store(msg.delivery_priority(),
                                        std::memory_order_release);
            observation_.flags.store(msg.delivery_flags(),
                                     std::memory_order_release);
        });
    }

  private:
    DeliveryMetadataObservation& observation_;
};
} // namespace

TEST(MessageIntegrationTest, DeliveryPipelineStampsPriorityAndFlags) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);
    DeliveryMetadataObservation observation;
    auto actor = system.spawn<DeliveryMetadataProbe>(observation);

    test::SchedulerTestDriver driver(system);

    mailbox::DeliveryOptions options;
    options.flags = 0x00000040u;
    auto result = system.try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{9}), 2, INT64_MAX,
        options);
    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(driver.drain_until([&] {
        return observation.flags.load(std::memory_order_acquire) != 0;
    }));
    EXPECT_EQ(observation.priority.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(observation.flags.load(std::memory_order_acquire), 0x40u);
}

} // anonymous namespace
} // namespace hpactor
