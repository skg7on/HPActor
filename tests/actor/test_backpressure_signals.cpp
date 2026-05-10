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

#include <cassert>

using namespace hpactor;

void test_backpressure_signal_on_soft_pressure() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    // Small capacity with low high-watermark to trigger soft pressure quickly
    cfg.mailbox.default_capacity = 2;
    cfg.mailbox.high_watermark = 0.50;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();

    // Get the sender's ActorContext that is stored in actor_contexts_
    // (created during spawn). The signal_backpressure() lookup uses this map.
    auto* sender_local = static_cast<LocalActor*>(sender.get().get());
    auto* sender_ctx = sender_local->context();
    assert(sender_ctx != nullptr);

    bool signaled = false;
    sender_ctx->on_backpressure([&](const mailbox::BackpressureSignal& signal) {
        signaled = true;
        assert(signal.target.id == target.id());
        assert(signal.sender.id == sender.id());
        assert(signal.reason == mailbox::BackpressureReason::HighWatermark);
        assert(signal.depth == 1);
        assert(signal.capacity == 2);
        assert(signal.pressure_ratio >= 0.5);
    });

    // Send one message: depth 1 of capacity 2 = 0.5, meets high_watermark
    auto result = sender_ctx->try_send(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    assert(result.accepted());
    assert(result.code == mailbox::EnqueueResultCode::AcceptedWithSoftPressure);
    assert(signaled);
}

void test_no_signal_when_emit_backpressure_disabled() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.mailbox.default_capacity = 2;
    cfg.mailbox.high_watermark = 0.50;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();

    auto* sender_local = static_cast<LocalActor*>(sender.get().get());
    auto* sender_ctx = sender_local->context();

    bool signaled = false;
    sender_ctx->on_backpressure(
        [&](const mailbox::BackpressureSignal& /*signal*/) { signaled = true; });

    mailbox::DeliveryOptions options;
    options.emit_backpressure = false;

    auto result = sender_ctx->try_send(
        target.address(), TypedMessage(TypeTag::User, StreamBuffer{1}), options);
    assert(result.accepted());
    // Message was accepted but no backpressure signal should fire
    assert(!signaled);
}

void test_no_signal_when_below_watermark() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    // Large capacity, message won't cross high watermark
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
    assert(result.accepted());
    assert(result.code == mailbox::EnqueueResultCode::Accepted);
    assert(!signaled);
}

int main() {
    test_backpressure_signal_on_soft_pressure();
    test_no_signal_when_emit_backpressure_disabled();
    test_no_signal_when_below_watermark();
    return 0;
}
