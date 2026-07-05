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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/lifecycle/quarantine_reason.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/cli_messages.pb.h>
#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace hpactor;

// =============================================================================
// TestEventHandler — minimal EventBasedActor + LifecycleActor for testing
// proto handler registration, dispatch, and reply behavior.
// =============================================================================
class TestEventHandler : public EventBasedActor, public LifecycleActor {
  public:
    int on_count = 0;
    int request_count = 0;
    int behavior_count = 0;
    MetricsRequest last_request;
    TypeTag last_received_tag = TypeTag::Invalid;
    std::string become_trace;

    /// Optional hook called from register_handlers() so each test can
    /// install its own handlers without subclassing.
    std::function<void(TestEventHandler*)> register_hook;

    TestEventHandler(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    void register_handlers() override {
        if (register_hook) {
            register_hook(this);
        }
    }
};

// =============================================================================
// EventBasedActorTest — fixture with scheduler_threads=0 for deterministic
// message injection and synchronous receive() calls.
// =============================================================================
class EventBasedActorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
        cfg.enable_network = false;
        cfg.cli.enabled = false;
        cfg.tracing.enabled = false;
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

    /// Spawn a TestEventHandler and return a raw pointer. The actor stays
    /// alive in the system registry until TearDown shutdown.
    TestEventHandler* spawn_test_actor() {
        auto actor_ref = system_->spawn<TestEventHandler>();
        return static_cast<TestEventHandler*>(actor_ref.get().get());
    }

    /// Inject a raw TypedMessage (tag + serialized bytes) into the actor's
    /// mailbox without scheduler notification.
    void inject_message(EventBasedActor* actor, TypeTag tag,
                        const StreamBuffer& payload,
                        const ActorAddress& sender = ActorAddress{}) {
        auto* mailbox = actor->get_mailbox();
        ASSERT_NE(mailbox, nullptr);
        auto* node = static_cast<TypedMessage*>(mem::allocate(
            mem::RegionType::kMessage, sizeof(TypedMessage), actor->id()));
        ASSERT_NE(node, nullptr);
        new (node) TypedMessage(tag, StreamBuffer(payload));
        node->set_sender_address(sender);
        mailbox->inject_for_test(node);
    }

    /// Serialize a protobuf message, inject it, pop it, and deliver it via
    /// receive().  Equivalent to what the scheduler does when it dequeues a
    /// message on the actor's worker thread.
    template <typename ProtoMsg>
    void inject_and_receive(EventBasedActor* actor, TypeTag tag,
                            const ProtoMsg& proto_msg,
                            const ActorAddress& sender = ActorAddress{}) {
        StreamBuffer payload(proto_msg.ByteSizeLong());
        (void)proto_msg.SerializeToArray(payload.data(),
                                         static_cast<int>(payload.size()));
        inject_message(actor, tag, payload, sender);
        TypedMessage msg;
        bool popped = actor->get_mailbox()->try_pop(msg);
        ASSERT_TRUE(popped);
        actor->receive(msg);
    }

    std::unique_ptr<ActorSystem> system_;
};

// =============================================================================
// Test 1: on<T>() invokes handler for matching tag
// =============================================================================
TEST_F(EventBasedActorTest, OnInvokesHandlerForMatchingTag) {
    auto* actor = spawn_test_actor();
    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);

    EXPECT_EQ(actor->on_count, 1);
}

// =============================================================================
// Test 2: on<T>() does NOT fire for a different (non-matching) tag
// =============================================================================
TEST_F(EventBasedActorTest, OnDoesNotFireForDifferentTag) {
    auto* actor = spawn_test_actor();
    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    // Send MetricsResponse with MetricsResponseTag — should NOT trigger the
    // MetricsRequest handler.
    MetricsResponse resp;
    resp.set_body("test");
    inject_and_receive(actor, TypeTag::MetricsResponseTag, resp);

    EXPECT_EQ(actor->on_count, 0);
}

// =============================================================================
// Test 3: on_request<ReqT, ResT>() serializes the response and sends a reply
//         to the original sender's mailbox.
// =============================================================================
TEST_F(EventBasedActorTest, OnRequestSerializesAndReplies) {
    auto* actor = spawn_test_actor();
    // reply_target receives the reply
    auto* reply_target = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on_request<MetricsRequest, MetricsResponse>(
            [a](const MetricsRequest& req) -> MetricsResponse {
                a->request_count++;
                a->last_request = req;
                MetricsResponse resp;
                resp.set_body("reply_body");
                return resp;
            });
    };

    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req,
                       reply_target->address());

    // Handler was invoked once
    EXPECT_EQ(actor->request_count, 1);

    // A reply message should have been delivered to reply_target's mailbox
    auto* reply_mbox = reply_target->get_mailbox();
    ASSERT_NE(reply_mbox, nullptr);

    TypedMessage reply_msg;
    ASSERT_TRUE(reply_mbox->try_pop(reply_msg));
    EXPECT_EQ(reply_msg.type_id(), TypeTag::MetricsRequestTag);

    MetricsResponse received_resp;
    ASSERT_TRUE(received_resp.ParseFromArray(
        reply_msg.payload().data(), static_cast<int>(reply_msg.payload().size())));
    EXPECT_EQ(received_resp.body(), "reply_body");
}

// =============================================================================
// Test 4: on_request returns no reply when the response ByteSizeLong() is 0
//         (empty protobuf message → empty StreamBuffer → no ctx->reply()).
// =============================================================================
TEST_F(EventBasedActorTest, OnRequestNoReplyWhenResponseEmpty) {
    auto* actor = spawn_test_actor();
    auto* reply_target = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on_request<MetricsRequest, MetricsResponse>(
            [](const MetricsRequest&) -> MetricsResponse {
                // Default-constructed MetricsResponse has ByteSizeLong() == 0
                return MetricsResponse{};
            });
    };

    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req,
                       reply_target->address());

    // The handler was invoked — verify by having sent a request to a second
    // actor that logs, but here we just check no reply was queued.
    auto* reply_mbox = reply_target->get_mailbox();
    ASSERT_NE(reply_mbox, nullptr);

    TypedMessage reply_msg;
    EXPECT_FALSE(reply_mbox->try_pop(reply_msg)) << "empty response should not "
                                                    "trigger reply";
}

// =============================================================================
// Test 5: handles() returns true for a registered (already-dispatched) tag
// =============================================================================
TEST_F(EventBasedActorTest, HandlesReturnsTrueForRegisteredTag) {
    auto* actor = spawn_test_actor();
    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([](const MetricsRequest&) {});
    };

    // One dispatch to trigger initialize_proto_handlers()
    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);

    EXPECT_TRUE(actor->handles(TypeTag::MetricsRequestTag));
}

// =============================================================================
// Test 6: handles() returns false for tags that were never registered
// =============================================================================
TEST_F(EventBasedActorTest, HandlesReturnsFalseForUnregisteredTag) {
    auto* actor = spawn_test_actor();
    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([](const MetricsRequest&) {});
    };

    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);

    // MetricsResponseTag is NOT registered
    EXPECT_FALSE(actor->handles(TypeTag::MetricsResponseTag));
    // User tag range — definitely not registered
    EXPECT_FALSE(actor->handles(TypeTag::User));
}

// =============================================================================
// Test 7: handlers_initialized_ remains false until the first message triggers
//         initialize_proto_handlers().
// =============================================================================
TEST_F(EventBasedActorTest, HandlersInitializedLazily) {
    auto* actor = spawn_test_actor();

    bool initialized = false;
    actor->register_hook = [&initialized](TestEventHandler* a) {
        initialized = true;
        a->on<MetricsRequest>([](const MetricsRequest&) {});
    };

    // Before any dispatch, handlers should NOT be initialized
    EXPECT_FALSE(initialized);

    // First dispatch triggers initialization
    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);

    EXPECT_TRUE(initialized);
}

// =============================================================================
// Test 8: multiple on<T>() handlers for different tags each fire correctly
//         without cross-talk.
// =============================================================================
TEST_F(EventBasedActorTest, MultipleHandlersForDifferentTags) {
    auto* actor = spawn_test_actor();
    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
        a->on<MetricsResponse>(
            [a](const MetricsResponse&) { a->request_count++; });
    };

    // Dispatch MetricsRequest — on_count increments, request_count unchanged
    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->on_count, 1);
    EXPECT_EQ(actor->request_count, 0);

    // Dispatch MetricsResponse — request_count increments, on_count unchanged
    MetricsResponse resp;
    resp.set_body("test");
    inject_and_receive(actor, TypeTag::MetricsResponseTag, resp);
    EXPECT_EQ(actor->on_count, 1);
    EXPECT_EQ(actor->request_count, 1);
}

// ═══════════════════════════════════════════════════════════════════
// Section 2: become / become_empty
// ═══════════════════════════════════════════════════════════════════

TEST_F(EventBasedActorTest, BecomeReplacesCurrentBehavior) {
    auto* actor = spawn_test_actor();

    // Install behavior A
    actor->become(
        Behavior{[actor](TypedMessage& /*msg*/) { actor->become_trace += "A"; }});

    // Inject a message with unknown TypeTag so it falls through to behavior
    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->become_trace, "A");

    // Install behavior B
    actor->become(
        Behavior{[actor](TypedMessage& /*msg*/) { actor->become_trace += "B"; }});

    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->become_trace, "AB");
}

TEST_F(EventBasedActorTest, BecomeEmptyDropsMessages) {
    auto* actor = spawn_test_actor();

    // Set up a behavior first
    actor->become(
        Behavior{[actor](TypedMessage& /*msg*/) { actor->behavior_count++; }});

    // Verify behavior fires
    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->behavior_count, 1);

    // Now become_empty — subsequent messages should not invoke behavior
    actor->become_empty();

    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->behavior_count, 1);
}

TEST_F(EventBasedActorTest, BecomeFromWithinHandler) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) {
            a->become_trace += "proto";
            a->become(Behavior{[a](TypedMessage& /*msg*/) {
                a->become_trace += ":behavior";
            }});
        });
    };

    // First message: hits proto handler, which calls become()
    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->become_trace, "proto");

    // Second message with same tag: hits proto handler AGAIN
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->become_trace, "protoproto");

    // Third message with unknown tag: falls through to behavior
    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->become_trace, "protoproto:behavior");
}

TEST_F(EventBasedActorTest, RepeatedBecomeCycle) {
    auto* actor = spawn_test_actor();

    // Chain: A → B → C → empty
    actor->become(Behavior{[actor](TypedMessage& /*msg*/) {
        actor->become_trace += "A";
        actor->become(Behavior{[actor](TypedMessage& /*msg*/) {
            actor->become_trace += "B";
            actor->become(Behavior{[actor](TypedMessage& /*msg*/) {
                actor->become_trace += "C";
                actor->become_empty();
            }});
        }});
    }});

    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->become_trace, "A");

    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->become_trace, "AB");

    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->become_trace, "ABC");

    // Fourth message: empty behavior = no-op
    inject_message(actor, TypeTag(0x9999), StreamBuffer{});
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->become_trace, "ABC");
}

// ═══════════════════════════════════════════════════════════════════
// Section 3: receive() dispatch priority
// ═══════════════════════════════════════════════════════════════════

TEST_F(EventBasedActorTest, ProtoHandlerPriorityOverBehavior) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };
    actor->become(
        Behavior{[actor](TypedMessage& /*msg*/) { actor->behavior_count++; }});

    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);

    EXPECT_EQ(actor->on_count, 1);
    EXPECT_EQ(actor->behavior_count, 0);
}

TEST_F(EventBasedActorTest, BehaviorFallbackForUnknownTag) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };
    actor->become(
        Behavior{[actor](TypedMessage& /*msg*/) { actor->behavior_count++; }});

    // Known tag → proto handler fires
    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->on_count, 1);
    EXPECT_EQ(actor->behavior_count, 0);

    // Unknown tag → behavior fallback
    inject_message(actor, TypeTag::User, StreamBuffer{42});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
    actor->receive(msg);
    EXPECT_EQ(actor->on_count, 1);
    EXPECT_EQ(actor->behavior_count, 1);
}

TEST_F(EventBasedActorTest, NoOpForUnknownTagAndEmptyBehavior) {
    auto* actor = spawn_test_actor();

    // No handlers, no behavior — receive on unknown tag should be a safe no-op
    inject_message(actor, TypeTag::User, StreamBuffer{42});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));
}

// ═══════════════════════════════════════════════════════════════════
// Section 4: Error paths and edge cases
// ═══════════════════════════════════════════════════════════════════

TEST_F(EventBasedActorTest, DeserializationFailureIsSafe) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    // Corrupted payload — ParseFromArray should fail for MetricsRequest
    StreamBuffer corrupt = {0xFF, 0xFF, 0xFF, 0xFF};
    inject_message(actor, TypeTag::MetricsRequestTag, corrupt);
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));
    EXPECT_EQ(actor->on_count, 0);
}

TEST_F(EventBasedActorTest, UnknownTypeTagNoSideEffects) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    // Initialize handlers
    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->on_count, 1);

    // Send a completely unknown tag
    inject_message(actor, TypeTag(0xDEAD), StreamBuffer{});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));
    EXPECT_EQ(actor->on_count, 1);
}

TEST_F(EventBasedActorTest, TwoConsecutiveMessagesBothHandled) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->on_count, 1);

    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->on_count, 2);
}

TEST_F(EventBasedActorTest, EmptyPayloadSafe) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    // Empty payload — MetricsRequest has ByteSizeLong() == 0, so this is valid
    StreamBuffer empty_payload;
    inject_message(actor, TypeTag::MetricsRequestTag, empty_payload);
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));
    EXPECT_EQ(actor->on_count, 1);
}

// ═══════════════════════════════════════════════════════════════════
// Section 5: System message interception
// ═══════════════════════════════════════════════════════════════════

TEST_F(EventBasedActorTest, LinkMsgInterceptedBeforeProtoHandler) {
    auto* actor = spawn_test_actor();
    auto* other = spawn_test_actor();

    DownMessage pb;
    pb.set_actor_id(other->id().value());
    pb.set_reason_code(0);

    StreamBuffer payload(pb.ByteSizeLong());
    (void)pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    inject_message(actor, TypeTag::LinkMsg, payload, other->address());
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));

    auto* ctx = actor->context();
    ASSERT_NE(ctx, nullptr);
    bool found = false;
    for (const auto& linked : ctx->linked_actors()) {
        if (linked.id == other->id()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(EventBasedActorTest, DownMsgCleansUpLinkedMonitored) {
    auto* actor = spawn_test_actor();
    auto* other = spawn_test_actor();

    // First link 'other' to 'actor' via LinkMsg
    {
        DownMessage pb;
        pb.set_actor_id(other->id().value());
        StreamBuffer payload(pb.ByteSizeLong());
        (void)pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));
        inject_message(actor, TypeTag::LinkMsg, payload, other->address());
        TypedMessage msg;
        ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));
        actor->receive(msg);
    }

    // Now send DownMsg — should remove 'other' from linked_actors
    DownMessage down_pb;
    down_pb.set_actor_id(other->id().value());
    down_pb.set_reason_code(42);
    StreamBuffer payload(down_pb.ByteSizeLong());
    (void)down_pb.SerializeToArray(payload.data(),
                                   static_cast<int>(payload.size()));

    inject_message(actor, TypeTag::DownMsg, payload, other->address());
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));

    auto* ctx = actor->context();
    ASSERT_NE(ctx, nullptr);
    for (const auto& linked : ctx->linked_actors()) {
        EXPECT_NE(linked.id, other->id());
    }
}

TEST_F(EventBasedActorTest, MonitorMsgRegistration) {
    auto* actor = spawn_test_actor();
    auto* other = spawn_test_actor();

    DownMessage pb;
    pb.set_actor_id(other->id().value());
    StreamBuffer payload(pb.ByteSizeLong());
    (void)pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    inject_message(actor, TypeTag::MonitorMsg, payload, other->address());
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));

    auto* ctx = actor->context();
    ASSERT_NE(ctx, nullptr);
    bool found = false;
    for (const auto& m : ctx->monitored_actors()) {
        if (m.id == other->id()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ═══════════════════════════════════════════════════════════════════
// Section 6: Lifecycle gate
// ═══════════════════════════════════════════════════════════════════

TEST_F(EventBasedActorTest, QuarantinedActorRejectsUserMessages) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    // Transition to quarantined
    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    lc->transition_to_quarantined(QuarantineReason::CircuitBreakerTrip);
    ASSERT_TRUE(lc->is_quarantined());

    // User-range tag (>= 0x1000) should be rejected by lifecycle gate
    inject_message(actor, TypeTag::User, StreamBuffer{42});
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    actor->receive(msg);
    EXPECT_EQ(actor->on_count, 0);
}

TEST_F(EventBasedActorTest, ActiveActorAcceptsUserMessages) {
    auto* actor = spawn_test_actor();

    actor->register_hook = [](TestEventHandler* a) {
        a->on<MetricsRequest>([a](const MetricsRequest&) { a->on_count++; });
    };

    // Ensure the actor is in Active state for message acceptance
    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    if (lc->state() == LifecycleState::kStarting) {
        lc->transition(LifecycleState::kActive);
    }

    MetricsRequest req;
    inject_and_receive(actor, TypeTag::MetricsRequestTag, req);
    EXPECT_EQ(actor->on_count, 1);
}

// ═══════════════════════════════════════════════════════════════════
// Section 7: CLI dispatch
// ═══════════════════════════════════════════════════════════════════

TEST_F(EventBasedActorTest, InspectStateRequestReturnsMetadata) {
    auto* actor = spawn_test_actor();
    ASSERT_NE(actor, nullptr);

    // Ensure the actor is in Active state before processing.  A newly
    // spawned actor starts in kStarting; some internal paths (mailbox
    // snapshot, metadata collection) may access state that is only fully
    // wired after the kStarting -> kActive transition.
    auto* actor_lc = actor->as_lifecycle();
    ASSERT_NE(actor_lc, nullptr);
    if (actor_lc->state() == LifecycleState::kStarting) {
        actor_lc->transition(LifecycleState::kActive);
    }

    cli::InspectStateRequest req;
    req.set_include_mailbox(true);
    req.set_include_state(false);

    StreamBuffer payload(req.ByteSizeLong());
    (void)req.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    auto* reply_target = spawn_test_actor();
    ASSERT_NE(reply_target, nullptr);

    auto* reply_lc = reply_target->as_lifecycle();
    ASSERT_NE(reply_lc, nullptr);
    if (reply_lc->state() == LifecycleState::kStarting) {
        reply_lc->transition(LifecycleState::kActive);
    }

    inject_message(actor, TypeTag::InspectStateRequestTag, payload,
                   reply_target->address());
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    // Verify the actor context is available before calling receive().
    // Under rare conditions (e.g. coverage-instrumented builds on
    // resource-constrained CI runners), the context may not be fully
    // wired; an explicit check turns a potential SEGFAULT into a clear
    // test failure.
    ASSERT_NE(actor->context(), nullptr);
    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));

    auto* reply_mbox = reply_target->get_mailbox();
    ASSERT_NE(reply_mbox, nullptr);
    TypedMessage reply_msg;
    ASSERT_TRUE(reply_mbox->try_pop(reply_msg));
    EXPECT_EQ(reply_msg.type_id(), TypeTag::InspectStateResponseTag);

    cli::InspectStateReply reply;
    ASSERT_TRUE(reply.ParseFromArray(reply_msg.payload().data(),
                                     static_cast<int>(reply_msg.payload().size())));
    EXPECT_TRUE(reply.has_metadata());
    EXPECT_GT(reply.metadata().actor_id(), 0u);
}

TEST_F(EventBasedActorTest, KillRequestDrivesLifecycleToStopped) {
    auto* actor = spawn_test_actor();

    cli::KillRequest req;
    StreamBuffer payload(req.ByteSizeLong());
    (void)req.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    auto* reply_target = spawn_test_actor();
    inject_message(actor, TypeTag::KillRequestTag, payload,
                   reply_target->address());
    TypedMessage msg;
    ASSERT_TRUE(actor->get_mailbox()->try_pop(msg));

    EXPECT_NO_FATAL_FAILURE(actor->receive(msg));

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);

    auto* reply_mbox = reply_target->get_mailbox();
    ASSERT_NE(reply_mbox, nullptr);
    TypedMessage reply_msg;
    ASSERT_TRUE(reply_mbox->try_pop(reply_msg));
    EXPECT_EQ(reply_msg.type_id(), TypeTag::KillResponseTag);

    cli::KillReply reply;
    ASSERT_TRUE(reply.ParseFromArray(reply_msg.payload().data(),
                                     static_cast<int>(reply_msg.payload().size())));
    EXPECT_TRUE(reply.success());
}
