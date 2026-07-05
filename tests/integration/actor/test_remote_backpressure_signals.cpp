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

#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/mailbox/backpressure_signal_serialization.hpp>
#include <hpactor/msg/frame.hpp>

using namespace hpactor;

TEST(RemoteBackpressureSignalsTest, DeliverRemoteSignalInvokesLocalHandler) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto producer = system.spawn<EventBasedActor>();
    auto* producer_local = static_cast<LocalActor*>(producer.get().get());
    ASSERT_NE(producer_local->context(), nullptr);

    mailbox::BackpressureSignal observed;
    bool signaled = false;
    producer_local->context()->on_backpressure(
        [&](const mailbox::BackpressureSignal& signal) {
            observed = signal;
            signaled = true;
        });

    mailbox::BackpressureSignal signal;
    signal.sender = producer.address();
    signal.target = ActorAddress{endpoint_ops::parse_endpoint("127.0.0.1:9002"),
                                 ActorType{0}, ActorId{777}, 0};
    signal.reason = mailbox::BackpressureReason::HardCapacity;
    signal.depth = 10;
    signal.capacity = 10;
    signal.pressure_ratio = 1.0;
    signal.retry_after = std::chrono::milliseconds{200};
    signal.sequence = 44;

    net::WireFrame frame;
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(),
                  signal.target);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  signal.sender);
    frame.pb_envelope.mutable_data_frame()->set_type_tag(
        static_cast<uint32_t>(TypeTag::BackpressureSignalTag));
    frame.pb_envelope.mutable_data_frame()->set_message_id(signal.sequence);
    auto payload = mailbox::serialize_backpressure_signal(
        signal, mailbox::MailboxPressureState::HardPressure);
    frame.pb_envelope.mutable_data_frame()->set_payload(
        reinterpret_cast<const char*>(payload.data()), payload.size());

    system.deliver_remote(frame);

    EXPECT_TRUE(signaled);
    EXPECT_EQ(observed.sender.id, producer.id());
    EXPECT_EQ(observed.target.id, ActorId{777});
    EXPECT_EQ(observed.reason, mailbox::BackpressureReason::HardCapacity);
    EXPECT_EQ(observed.retry_after, std::chrono::milliseconds{200});
}

TEST(RemoteBackpressureSignalsTest, RemoteSenderReceivesControlFrame) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 1;
    cfg.mailbox.backpressure_mode = mailbox::BackpressureMode::RemoteSignal;
    cfg.mailbox.high_watermark = 0.50;
    ActorSystem system(cfg);

    bool sent = false;
    ActorAddress wire_receiver;
    StreamBuffer wire_payload;
    system.set_backpressure_signal_wire_sink_for_test(
        [&](const ActorAddress& receiver, const StreamBuffer& encoded) {
            sent = true;
            wire_receiver = receiver;
            wire_payload = encoded;
            return true;
        });

    auto target = system.spawn<EventBasedActor>();

    TypedMessage first(TypeTag::User, StreamBuffer{1});
    first.set_sender_address(ActorAddress{endpoint_ops::parse_endpoint("127.0."
                                                                       "0.1:"
                                                                       "9002"),
                                          ActorType{0}, ActorId{55}, 0});
    ASSERT_TRUE(system.try_deliver_local(target.id(), std::move(first)).accepted());

    TypedMessage second(TypeTag::User, StreamBuffer{2});
    second.set_sender_address(ActorAddress{endpoint_ops::parse_endpoint("127.0."
                                                                        "0.1:"
                                                                        "9002"),
                                           ActorType{0}, ActorId{55}, 0});
    auto rejected = system.try_deliver_local(target.id(), std::move(second));

    EXPECT_FALSE(rejected.accepted());
    EXPECT_TRUE(sent);
    EXPECT_EQ(wire_receiver.id, ActorId{55});

    auto frame = net::WireFrame::decode(wire_payload);
    EXPECT_EQ(static_cast<TypeTag>(frame.pb_envelope.data_frame().type_tag()),
              TypeTag::BackpressureSignalTag);
    StreamBuffer payload(frame.pb_envelope.data_frame().payload().begin(),
                         frame.pb_envelope.data_frame().payload().end());
    auto decoded = mailbox::deserialize_backpressure_signal(payload);
    ASSERT_TRUE(decoded.has_value());
    if (!decoded)
        return;
    EXPECT_EQ(decoded->signal.sender.id, ActorId{55});
    EXPECT_EQ(decoded->signal.target.id, target.id());
    // Reason is HighWatermark because the first accepted message already hit
    // HardPressure (depth 1/1). The rejection's signal is rate-limited.
    EXPECT_EQ(decoded->signal.pressure_ratio, 1.0);
    EXPECT_EQ(decoded->state, mailbox::MailboxPressureState::HardPressure);
}
