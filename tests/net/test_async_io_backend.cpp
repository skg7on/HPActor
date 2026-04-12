#include <hpactor/net/async_io_backend.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test OpType enum values
    assert(static_cast<uint32_t>(OpType::Send) == 1);
    assert(static_cast<uint32_t>(OpType::Recv) == 2);
    assert(static_cast<uint32_t>(OpType::Accept) == 3);
    assert(static_cast<uint32_t>(OpType::Connect) == 4);
    assert(static_cast<uint32_t>(OpType::TimerFired) == 5);
    assert(static_cast<uint32_t>(OpType::RecvFrom) == 6);
    assert(static_cast<uint32_t>(OpType::SendTo) == 7);

    // Test IoEvent flags can be combined with | and tested with &
    IoEvent combined = static_cast<IoEvent>(static_cast<uint32_t>(IoEvent::Read) | static_cast<uint32_t>(IoEvent::Write));
    assert((static_cast<uint32_t>(combined) & static_cast<uint32_t>(IoEvent::Read)) != 0);
    assert((static_cast<uint32_t>(combined) & static_cast<uint32_t>(IoEvent::Write)) != 0);
    assert((static_cast<uint32_t>(combined) & static_cast<uint32_t>(static_cast<IoEvent>(0))) == 0);

    // Test OpCompletion struct fields
    OpCompletion op;
    op.actor = ActorId(42);
    op.type = OpType::Send;
    op.fd = 7;
    op.result = 123;
    op.user_data = 999;
    assert(op.actor == ActorId(42));
    assert(op.type == OpType::Send);
    assert(op.fd == 7);
    assert(op.result == 123);
    assert(op.user_data == 999);

    // Test encode_user_data and decode_user_data are inverses
    int fd = 5;
    ActorId actor(12345);
    uint32_t op_type = static_cast<uint32_t>(OpType::Recv);

    uint64_t encoded = encode_user_data(fd, actor, op_type);
    int fd_out;
    ActorId actor_out;
    uint32_t op_type_out;
    decode_user_data(encoded, fd_out, actor_out, op_type_out);

    assert(fd_out == fd);
    assert(actor_out == actor);
    assert(op_type_out == op_type);

    return 0;
}
