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

#include <hpactor/net/frame.hpp>

#include <cassert>
#include <cstring>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test WireFrame default construction
    WireFrame f1;
    assert(f1.magic_hdr == WireFrame::MagicHeader);
    assert(f1.pb_frame.flags() == 0);
    assert(f1.pb_frame.message_id() == 0);

    // Test WireFrame with values
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          receiver_id, 6);

    WireFrame f2;
    to_proto(f2.pb_frame.mutable_sender(), sender);
    to_proto(f2.pb_frame.mutable_receiver(), receiver);
    f2.pb_frame.set_payload("hello", 5);
    f2.pb_frame.set_flags(WireFrame::Important);
    f2.pb_frame.set_message_id(12345);

    // Test encode/decode roundtrip
    StreamBuffer encoded = f2.encode();
    assert(!encoded.empty());

    WireFrame f3 = WireFrame::decode(encoded);
    auto decoded_sender = from_proto(f3.pb_frame.sender());
    auto decoded_receiver = from_proto(f3.pb_frame.receiver());
    assert(decoded_sender.endpoint == sender.endpoint);
    assert(decoded_sender.id.value() == sender.id.value());
    assert(decoded_sender.incarnation == sender.incarnation);
    assert(decoded_receiver.endpoint == receiver.endpoint);
    assert(decoded_receiver.id.value() == receiver.id.value());
    assert(decoded_receiver.incarnation == receiver.incarnation);
    assert(f3.pb_frame.payload() == f2.pb_frame.payload());
    assert(f3.pb_frame.flags() == f2.pb_frame.flags());
    assert(f3.pb_frame.message_id() == f2.pb_frame.message_id());

    // Test malformed data handling
    StreamBuffer malformed = {0xFF, 0xFF, 0xFF, 0xFF}; // Invalid magic
    WireFrame f_bad = WireFrame::decode(malformed);
    // Should return default frame, not crash
    assert(f_bad.magic_hdr == WireFrame::MagicHeader);
    assert(f_bad.pb_frame.message_id() == 0);

    // Test IPv6 endpoint
    std::array<uint8_t, 16> ipv6_addr = {0, 0, 0, 0, 0, 0, 0, 0,
                                         0, 0, 0, 0, 0, 0, 0, 1};
    ActorAddress ipv6_sender{Ipv6Endpoint{ipv6_addr, htons(8080)}, 1,
                             ActorId(300), 1};
    ActorAddress ipv6_receiver{Ipv6Endpoint{ipv6_addr, htons(9090)}, 2,
                               ActorId(400), 2};

    WireFrame f_ipv6;
    to_proto(f_ipv6.pb_frame.mutable_sender(), ipv6_sender);
    to_proto(f_ipv6.pb_frame.mutable_receiver(), ipv6_receiver);
    f_ipv6.pb_frame.set_payload("xyz", 3);
    f_ipv6.pb_frame.set_message_id(99999);

    StreamBuffer encoded_ipv6 = f_ipv6.encode();
    WireFrame decoded_ipv6 = WireFrame::decode(encoded_ipv6);

    assert(std::get<Ipv6Endpoint>(
               from_proto(decoded_ipv6.pb_frame.sender()).endpoint)
               .port_nw == htons(8080));
    assert(decoded_ipv6.pb_frame.message_id() == 99999);

    return 0;
}
