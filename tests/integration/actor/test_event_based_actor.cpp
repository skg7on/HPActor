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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

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
    MetricsRequest last_request;
    TypeTag last_received_tag = TypeTag::Invalid;

    /// Optional hook called from register_handlers() so each test can
    /// install its own handlers without subclassing.
    std::function<void(TestEventHandler*)> register_hook;

    TestEventHandler(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    LifecycleActor* as_lifecycle() override { return this; }
    const LifecycleActor* as_lifecycle() const override { return this; }

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
        reply_msg.payload().data(),
        static_cast<int>(reply_msg.payload().size())));
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
    EXPECT_FALSE(reply_mbox->try_pop(reply_msg))
        << "empty response should not trigger reply";
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
