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

#include <cstring>
#include <gtest/gtest.h>
#include <hpactor/net/frame.hpp>

using namespace hpactor;
using namespace hpactor::net;

TEST(FrameTest, WireFrameDefaultConstruction) {
    WireFrame f1;
    EXPECT_EQ(f1.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f1.pb_frame.flags(), 0);
    EXPECT_EQ(f1.pb_frame.message_id(), 0u);
}

TEST(FrameTest, WireFrameWithValues) {
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          receiver_id, 6);

    WireFrame f2;
    to_proto(f2.pb_frame.mutable_sender(), sender);
    to_proto(f2.pb_frame.mutable_receiver(), receiver);
    f2.pb_frame.set_payload("hello", 5u);
    f2.pb_frame.set_flags(WireFrame::Important);
    f2.pb_frame.set_message_id(12345);

    EXPECT_EQ(f2.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f2.pb_frame.payload(), "hello");
    EXPECT_EQ(f2.pb_frame.flags(), WireFrame::Important);
    EXPECT_EQ(f2.pb_frame.message_id(), 12345u);
}

TEST(FrameTest, EncodeDecodeRoundtrip) {
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          receiver_id, 6);

    WireFrame f2;
    to_proto(f2.pb_frame.mutable_sender(), sender);
    to_proto(f2.pb_frame.mutable_receiver(), receiver);
    f2.pb_frame.set_payload("hello", 5u);
    f2.pb_frame.set_flags(WireFrame::Important);
    f2.pb_frame.set_message_id(12345);

    StreamBuffer encoded = f2.encode();
    EXPECT_FALSE(encoded.empty());

    WireFrame f3 = WireFrame::decode(encoded);
    auto decoded_sender = from_proto(f3.pb_frame.sender());
    auto decoded_receiver = from_proto(f3.pb_frame.receiver());
    EXPECT_EQ(decoded_sender.endpoint, sender.endpoint);
    EXPECT_EQ(decoded_sender.id.value(), sender.id.value());
    EXPECT_EQ(decoded_sender.incarnation, sender.incarnation);
    EXPECT_EQ(decoded_receiver.endpoint, receiver.endpoint);
    EXPECT_EQ(decoded_receiver.id.value(), receiver.id.value());
    EXPECT_EQ(decoded_receiver.incarnation, receiver.incarnation);
    EXPECT_EQ(f3.pb_frame.payload(), f2.pb_frame.payload());
    EXPECT_EQ(f3.pb_frame.flags(), f2.pb_frame.flags());
    EXPECT_EQ(f3.pb_frame.message_id(), f2.pb_frame.message_id());
}

TEST(FrameTest, MalformedDataHandling) {
    StreamBuffer malformed = {0xFF, 0xFF, 0xFF, 0xFF};
    WireFrame f_bad = WireFrame::decode(malformed);
    EXPECT_EQ(f_bad.magic_hdr, WireFrame::MagicHeader);
    EXPECT_EQ(f_bad.pb_frame.message_id(), 0u);
}

TEST(FrameTest, Ipv6Endpoint) {
    std::array<uint8_t, 16> ipv6_addr = {0, 0, 0, 0, 0, 0, 0, 0,
                                         0, 0, 0, 0, 0, 0, 0, 1};
    ActorAddress ipv6_sender{Ipv6Endpoint{ipv6_addr, htons(8080)}, 1,
                             ActorId(300), 1};
    ActorAddress ipv6_receiver{Ipv6Endpoint{ipv6_addr, htons(9090)}, 2,
                               ActorId(400), 2};

    WireFrame f_ipv6;
    to_proto(f_ipv6.pb_frame.mutable_sender(), ipv6_sender);
    to_proto(f_ipv6.pb_frame.mutable_receiver(), ipv6_receiver);
    f_ipv6.pb_frame.set_payload("xyz", 3u);
    f_ipv6.pb_frame.set_message_id(99999);

    StreamBuffer encoded_ipv6 = f_ipv6.encode();
    WireFrame decoded_ipv6 = WireFrame::decode(encoded_ipv6);

    EXPECT_EQ(std::get<Ipv6Endpoint>(from_proto(decoded_ipv6.pb_frame.sender()).endpoint)
                  .port_nw,
              htons(8080));
    EXPECT_EQ(decoded_ipv6.pb_frame.message_id(), 99999u);
}
