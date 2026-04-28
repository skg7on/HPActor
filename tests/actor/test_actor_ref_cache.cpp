#include <hpactor/core/actor_ref_cache.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>

using namespace hpactor;

void test_empty_cache() {
    ActorRefCache cache;
    assert(!cache.get(ActorId{1}).has_value());
}

void test_put_and_get() {
    ActorRefCache cache;
    ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{1}, 0};
    ActorProxy proxy(addr, nullptr);
    ActorRef ref(std::move(proxy));

    cache.put(ActorId{1}, ref);

    auto result = cache.get(ActorId{1});
    assert(result.has_value());
    assert(!result->is_local());
    assert(result->address().id == ActorId{1});
}

void test_put_updates_existing() {
    ActorRefCache cache;
    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{1}, 0};
    ActorAddress addr2{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{2}, ActorId{1}, 0};

    ActorProxy proxy1(addr1, nullptr);
    ActorRef ref1(std::move(proxy1));
    cache.put(ActorId{1}, ref1);

    ActorProxy proxy2(addr2, nullptr);
    ActorRef ref2(std::move(proxy2));
    cache.put(ActorId{1}, ref2);  // overwrite

    auto result = cache.get(ActorId{1});
    assert(result.has_value());
    assert(result->address().type == ActorType{2});  // updated
}

void test_eviction_at_max() {
    ActorRefCache cache(3);  // max 3 entries

    for (uint64_t i = 1; i <= 3; ++i) {
        ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{i}, 0};
        ActorProxy proxy(addr, nullptr);
        ActorRef ref(std::move(proxy));
        cache.put(ActorId{i}, ref);
    }

    // All 3 should be present
    assert(cache.get(ActorId{1}).has_value());
    assert(cache.get(ActorId{2}).has_value());
    assert(cache.get(ActorId{3}).has_value());

    // Insert 4th — should evict least recently used (id=1, accessed once at insert)
    ActorAddress addr4{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{4}, 0};
    ActorProxy proxy4(addr4, nullptr);
    ActorRef ref4(std::move(proxy4));
    cache.put(ActorId{4}, ref4);

    // id=1 was LRU, should be gone
    assert(!cache.get(ActorId{1}).has_value());
    assert(cache.get(ActorId{2}).has_value());
    assert(cache.get(ActorId{3}).has_value());
    assert(cache.get(ActorId{4}).has_value());
}

void test_lru_access_updates_tick() {
    ActorRefCache cache(3);

    for (uint64_t i = 1; i <= 3; ++i) {
        ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{i}, 0};
        ActorProxy proxy(addr, nullptr);
        ActorRef ref(std::move(proxy));
        cache.put(ActorId{i}, ref);
    }

    // Access id=1 (making it recently used), so id=2 becomes LRU
    assert(cache.get(ActorId{1}).has_value());

    // Insert 4th — should evict id=2 (now LRU)
    ActorAddress addr4{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{4}, 0};
    ActorProxy proxy4(addr4, nullptr);
    ActorRef ref4(std::move(proxy4));
    cache.put(ActorId{4}, ref4);

    assert(cache.get(ActorId{1}).has_value());
    assert(!cache.get(ActorId{2}).has_value());  // evicted
    assert(cache.get(ActorId{3}).has_value());
    assert(cache.get(ActorId{4}).has_value());
}

int main() {
    test_empty_cache();
    test_put_and_get();
    test_put_updates_existing();
    test_eviction_at_max();
    test_lru_access_updates_tick();
    return 0;
}
