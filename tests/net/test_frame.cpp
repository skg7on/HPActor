#include <hpactor/net/frame.hpp>

#include <cassert>
#include <cstring>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test Frame default construction
    Frame f1;
    assert(f1.sender.node_id == 0);
    assert(f1.receiver.node_id == 0);
    assert(f1.payload.empty());
    assert(f1.flags == 0);
    assert(f1.message_id == 0);

    // Test Frame with values
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(1, 10, sender_id, 5);
    ActorAddress receiver(2, 20, receiver_id, 6);

    Frame f2;
    f2.sender = sender;
    f2.receiver = receiver;
    f2.payload = {1, 2, 3, 4, 5};
    f2.flags = Frame::Important;
    f2.message_id = 12345;

    // Test encode/decode roundtrip
    bytes encoded = f2.encode();
    assert(!encoded.empty());

    Frame f3 = Frame::decode(encoded);
    assert(f3.sender.node_id == sender.node_id);
    assert(f3.sender.id.value() == sender.id.value());
    assert(f3.sender.incarnation == sender.incarnation);
    assert(f3.receiver.node_id == receiver.node_id);
    assert(f3.receiver.id.value() == receiver.id.value());
    assert(f3.receiver.incarnation == receiver.incarnation);
    assert(f3.payload == f2.payload);
    assert(f3.flags == f2.flags);
    assert(f3.message_id == f2.message_id);

    return 0;
}
