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

// Tests full spawn flow: Frame encode/decode, spawn message correlation

#include <cstring>

#include <gtest/gtest.h>

#include <hpactor/actor/spawn.hpp>
#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>
#include <hpactor/msg/frame.hpp>
#include <hpactor/msg/proto_type_registry.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;

TEST(SpawnIntegrationTest, FrameEncoding) {
    net::WireFrame frame;
    auto snd_addr = ActorAddress{endpoint_ops::parse_endpoint("node1:12345"),
                                 ActorType{10}, ActorId{42}, 1};
    auto rcv_addr = ActorAddress{endpoint_ops::parse_endpoint("node2:12345"),
                                 SystemActorType, SpawnReceiverId, 0};

    net::to_proto(frame.pb_frame.mutable_sender(), snd_addr);
    net::to_proto(frame.pb_frame.mutable_receiver(), rcv_addr);
    frame.pb_frame.set_message_id(12345);
    frame.pb_frame.set_flags(net::WireFrame::RpcRequest);

    StreamBuffer encoded = frame.encode();
    EXPECT_GT(encoded.size(), 0u);

    net::WireFrame decoded = net::WireFrame::decode(encoded);

    auto dec_sender = net::from_proto(decoded.pb_frame.sender());
    auto dec_receiver = net::from_proto(decoded.pb_frame.receiver());
    EXPECT_EQ(dec_sender.endpoint, snd_addr.endpoint);
    EXPECT_EQ(dec_sender.id, snd_addr.id);
    EXPECT_EQ(dec_sender.incarnation, snd_addr.incarnation);
    EXPECT_EQ(dec_receiver.endpoint, rcv_addr.endpoint);
    EXPECT_EQ(dec_receiver.id, rcv_addr.id);
    EXPECT_EQ(dec_receiver.incarnation, rcv_addr.incarnation);
    EXPECT_EQ(decoded.pb_frame.message_id(), frame.pb_frame.message_id());
    EXPECT_EQ(decoded.pb_frame.flags(), frame.pb_frame.flags());
}

TEST(SpawnIntegrationTest, SpawnRequestProtobuf) {
    ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("worker");
    pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
    pb_req.set_serialized_args("abc");
    auto* sup = pb_req.mutable_supervisor();
    sup->mutable_global_addr()->mutable_endpoint()->mutable_ipv4()->set_addr(
        0x7F000001);
    sup->mutable_global_addr()->mutable_endpoint()->mutable_ipv4()->set_port(8080);
    sup->mutable_global_addr()->mutable_local_addr()->set_actor_type(10);
    sup->mutable_global_addr()->mutable_local_addr()->set_actor_id(42);
    sup->mutable_global_addr()->mutable_local_addr()->set_incarnation(1);

    StreamBuffer encoded = registry.serialize(pb_req);
    EXPECT_GT(encoded.size(), 0u);

    auto decoded = registry.deserialize(TypeTag::SpawnRequestTag, encoded);
    ASSERT_NE(decoded, nullptr);

    auto* decoded_req =
        static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    EXPECT_EQ(decoded_req->actor_type_name(), "worker");
    EXPECT_EQ(decoded_req->args_type(), static_cast<uint32_t>(TypeTag::User));
    EXPECT_EQ(decoded_req->serialized_args(), "abc");
    EXPECT_EQ(decoded_req->supervisor().global_addr().local_addr().actor_id(), 42);
}

TEST(SpawnIntegrationTest, MessageIdCorrelation) {
    uint64_t request_message_id = generate_message_id().value();

    net::WireFrame request_frame;
    request_frame.pb_frame.set_message_id(request_message_id);
    request_frame.pb_frame.set_flags(net::WireFrame::RpcRequest);

    net::WireFrame response_frame;
    response_frame.pb_frame.set_message_id(request_message_id);
    response_frame.pb_frame.set_flags(net::WireFrame::RpcResponse);

    EXPECT_EQ(response_frame.pb_frame.message_id(),
              request_frame.pb_frame.message_id());

    uint64_t matched_id = response_frame.pb_frame.message_id();
    EXPECT_EQ(matched_id, request_message_id);
}