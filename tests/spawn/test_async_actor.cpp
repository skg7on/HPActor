#include <hpactor/spawn.hpp>
#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <thread>
#include <cassert>

using namespace hpactor;

void test_async_actor_default_constructor() {
    AsyncActor handle;
    assert(!handle.ready());
    assert(handle.node_id() == 0);
}

void test_async_actor_constructor() {
    AsyncActor handle(NodeId{42}, std::chrono::milliseconds{1000});
    assert(handle.node_id() == 42);
    assert(!handle.ready());
}

void test_async_actor_get_timeout() {
    AsyncActor handle(NodeId{1}, std::chrono::milliseconds{50});
    auto result = handle.get();
    assert(!result.has_value());  // should timeout
    assert(result.error().code() == errors::timeout);
}

void test_async_actor_response_set() {
    AsyncActor handle(NodeId{1}, std::chrono::milliseconds{100});

    // Simulate response received
    SpawnResponse resp;
    resp.actor_addr = ActorAddress{1, 100, ActorId{1}, 0};
    resp.error_code = spawn_errors::success;
    handle.set_response(resp);

    assert(handle.ready());
    auto result = handle.get();
    assert(result.has_value());
    assert(result.value().node_id() == 1);
}

void test_async_actor_cancel() {
    AsyncActor handle(NodeId{1}, std::chrono::milliseconds{1000});
    handle.cancel();
    assert(handle.ready());  // cancelled appears as ready
    auto result = handle.get();
    assert(!result.has_value());
}

int main() {
    test_async_actor_default_constructor();
    test_async_actor_constructor();
    test_async_actor_get_timeout();
    test_async_actor_response_set();
    test_async_actor_cancel();
    return 0;
}