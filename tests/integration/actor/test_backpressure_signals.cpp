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
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

// Fixture for backpressure signals tests
class BackpressureSignalsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        // Small capacity with low high-watermark to trigger soft pressure
        // quickly
        cfg.mailbox.default_capacity = 2;
        cfg.mailbox.high_watermark = 0.50;
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

TEST_F(BackpressureSignalsTest, SignalOnSoftPressure) {
    auto sender = system_->spawn<EventBasedActor>();
    auto target = system_->spawn<EventBasedActor>();

    auto* sender_local = static_cast<LocalActor*>(sender.get().get());
    auto* sender_ctx = sender_local->context();
    ASSERT_NE(sender_ctx, nullptr);

    bool signaled = false;
    sender_ctx->on_backpressure([&](const mailbox::BackpressureSignal& signal) {
        signaled = true;
        EXPECT_EQ(signal.target.id, target.id());
        EXPECT_EQ(signal.sender.id, sender.id());
        EXPECT_EQ(signal.reason, mailbox::BackpressureReason::HighWatermark);
        EXPECT_EQ(signal.depth, 1u);
        EXPECT_EQ(signal.capacity, 2u);
        EXPECT_GE(signal.pressure_ratio, 0.5);
    });

    // Send one message: depth 1 of capacity 2 = 0.5, meets high_watermark
    auto result = sender_ctx->try_send(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::AcceptedWithSoftPressure);
    EXPECT_TRUE(signaled);
}

TEST_F(BackpressureSignalsTest, NoSignalWhenEmitBackpressureDisabled) {
    auto sender = system_->spawn<EventBasedActor>();
    auto target = system_->spawn<EventBasedActor>();

    auto* sender_local = static_cast<LocalActor*>(sender.get().get());
    auto* sender_ctx = sender_local->context();

    bool signaled = false;
    sender_ctx->on_backpressure(
        [&](const mailbox::BackpressureSignal& /*signal*/) { signaled = true; });

    mailbox::DeliveryOptions options;
    options.emit_backpressure = false;

    auto result = sender_ctx->try_send(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{1}), options);
    EXPECT_TRUE(result.accepted());
    EXPECT_FALSE(signaled);
}

TEST_F(BackpressureSignalsTest, NoSignalWhenBelowWatermark) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.mailbox.default_capacity = 100;
    cfg.mailbox.high_watermark = 0.80;
    ActorSystem system(cfg);

    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();

    auto* sender_local = static_cast<LocalActor*>(sender.get().get());
    auto* sender_ctx = sender_local->context();

    bool signaled = false;
    sender_ctx->on_backpressure(
        [&](const mailbox::BackpressureSignal& /*signal*/) { signaled = true; });

    auto result = sender_ctx->try_send(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::Accepted);
    EXPECT_FALSE(signaled);
}
