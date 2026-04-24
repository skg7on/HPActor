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
    // Test Frame default construction
    WireFrame f1;
    assert(f1.sender.endpoint == endpoint_ops::parse_endpoint(""));
    assert(f1.receiver.endpoint == endpoint_ops::parse_endpoint(""));
    assert(f1.payload.empty());
    assert(f1.flags == 0);
    assert(f1.message_id == 0);

    // Test Frame with values
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10, sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20, receiver_id, 6);

    WireFrame f2;
    f2.sender = sender;
    f2.receiver = receiver;
    f2.payload = {1, 2, 3, 4, 5};
    f2.flags = WireFrame::Important;
    f2.message_id = 12345;

    // Test encode/decode roundtrip
    bytes encoded = f2.encode();
    assert(!encoded.empty());

    WireFrame f3 = WireFrame::decode(encoded);
    assert(f3.sender.endpoint == sender.endpoint);
    assert(f3.sender.id.value() == sender.id.value());
    assert(f3.sender.incarnation == sender.incarnation);
    assert(f3.receiver.endpoint == receiver.endpoint);
    assert(f3.receiver.id.value() == receiver.id.value());
    assert(f3.receiver.incarnation == receiver.incarnation);
    assert(f3.payload == f2.payload);
    assert(f3.flags == f2.flags);
    assert(f3.message_id == f2.message_id);

    return 0;
}
