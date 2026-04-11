#include <cassert>
#include <hpactor/types.hpp>
#include <cstdint>

int main() {
    // Test 1: ActorId default construction (value == 0)
    hpactor::ActorId default_actor_id;
    assert(default_actor_id.value() == 0);

    // Test 2: ActorId explicit construction from counter_type
    hpactor::ActorId explicit_actor_id(42);
    assert(explicit_actor_id.value() == 42);

    // Test 3: ActorId equality
    hpactor::ActorId actor_id1(100);
    hpactor::ActorId actor_id2(100);
    hpactor::ActorId actor_id3(200);
    assert(actor_id1 == actor_id2);

    // Test 4: ActorId inequality
    assert(actor_id1 != actor_id3);

    // Test 5: ActorId value accessor
    assert(actor_id1.value() == 100);

    // Test 6: NodeId with InvalidNodeId
    hpactor::NodeId node = hpactor::InvalidNodeId;
    assert(node == hpactor::InvalidNodeId);

    // Test 7: ActorType with InvalidActorType
    hpactor::ActorType actor_type = hpactor::InvalidActorType;
    assert(actor_type == hpactor::InvalidActorType);

    // Test 8: error class
    hpactor::error ok_err;
    assert(ok_err.ok());
    assert(!ok_err);

    hpactor::error err(42, "test error");
    assert(!err.ok());
    assert(err);
    assert(err.code() == 42);
    assert(err.message() == "test error");

    // Test 9: errors namespace
    assert(hpactor::errors::unknown == 1);
    assert(hpactor::errors::actor_down == 2);
    assert(hpactor::errors::actor_not_found == 3);
    assert(hpactor::errors::mailbox_full == 4);
    assert(hpactor::errors::timeout == 5);
    assert(hpactor::errors::user == 1000);

    // Test 10: MessageId generate
    hpactor::MessageId id1 = hpactor::MessageId::generate();
    hpactor::MessageId id2 = hpactor::MessageId::generate();
    assert(id1 != id2); // Each call should be unique

    // Test 11: Clock
    hpactor::Clock::time_point tp = hpactor::Clock::now();
    hpactor::Clock::duration dur = hpactor::Clock::duration(100);
    hpactor::Clock::time_point tp2 = tp + dur;
    assert(tp2 > tp);

    // Test 12: AlarmHandle
    hpactor::AlarmHandle handle1;
    hpactor::AlarmHandle handle2(42);
    assert(handle1.id() == 0);
    assert(handle2.id() == 42);

    // Test 13: TraceContext
    hpactor::TraceContext ctx(1, 2, 3);
    assert(ctx.trace_id() == 1);
    assert(ctx.span_id() == 2);
    assert(ctx.flags() == 3);

    // Test 14: bytes
    hpactor::bytes data = {1, 2, 3, 4, 5};
    assert(data.size() == 5);
    assert(data[0] == 1);
    assert(data[4] == 5);

    return 0;
}