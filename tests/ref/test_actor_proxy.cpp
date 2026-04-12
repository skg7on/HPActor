#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/net/transport.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    // Test ActorProxy construction with invalid address
    ActorProxy proxy1(ActorAddress{}, nullptr);
    assert(!proxy1);
    assert(proxy1.address().node_id == 0);
    assert(proxy1.node_id() == 0);
    assert(!proxy1.is_local());

    // Test ActorProxy with valid address
    ActorId id(42);
    ActorAddress addr(1, 0, id, 0);  // remote node
    ActorProxy proxy2(addr, nullptr);
    assert(proxy2);
    assert(proxy2.address() == addr);
    assert(proxy2.node_id() == 1);
    assert(!proxy2.is_local());

    return 0;
}
