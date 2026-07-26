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

// tests/unit/ref/test_ref_branches.cpp
//
// Branch-coverage tests for actor reference subsystem: ActorRef variants,
// ActorProxy send routing, ActorAddress serialization, ActorRefCache,
// and Actor lifecycle.

#include <gtest/gtest.h>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/system/actor_ref_cache.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <thread>

using namespace hpactor;

// ============================================================================
// Actor — opaque handle to a local actor
// ============================================================================

TEST(ActorTest, DefaultConstructedIsInvalid) {
    Actor a;
    EXPECT_FALSE(a);
    EXPECT_EQ(a.id().value(), 0U);
    EXPECT_EQ(a.type(), 0U);
    EXPECT_FALSE(a.address());
}

TEST(ActorTest, ConstructFromSharedPtr) {
    // AbstractActor is interface; use nullptr to test empty handle
    auto ptr = std::shared_ptr<AbstractActor>(nullptr);
    Actor a(ptr);
    EXPECT_FALSE(a); // nullptr shared_ptr means empty handle
}

TEST(ActorTest, Swap) {
    Actor a1;
    Actor a2;
    EXPECT_FALSE(a1);
    EXPECT_FALSE(a2);

    a1.swap(a2);
    EXPECT_FALSE(a1);
    EXPECT_FALSE(a2);
}

TEST(ActorTest, GetReturnsUnderlyingPointer) {
    auto ptr = std::shared_ptr<AbstractActor>(nullptr);
    Actor a(ptr);
    EXPECT_EQ(a.get(), ptr);
}

// ============================================================================
// ActorAddress — additional IPv4/IPv6 serialization and hash tests
// ============================================================================

TEST(ActorAddressBranchesTest, DefaultIsLocal) {
    ActorAddress addr;
    EXPECT_TRUE(addr.is_local());
    EXPECT_FALSE(addr); // id is 0, so bool conversion is false
}

TEST(ActorAddressBranchesTest, Ipv4LoopbackIsLocal) {
    ActorId id(42);
    ActorAddress addr{Ipv4Endpoint{0x7F000001, 8080}, 1, id, 0};
    EXPECT_TRUE(addr.is_local());
    EXPECT_TRUE(addr); // valid id
}

TEST(ActorAddressBranchesTest, Ipv4NonLoopbackIsRemote) {
    ActorId id(42);
    // 192.168.1.1:9090
    Ipv4Endpoint ep{0xC0A80101, htons(9090)};
    ActorAddress addr{ep, 1, id, 0};
    EXPECT_FALSE(addr.is_local());
}

TEST(ActorAddressBranchesTest, Ipv6LoopbackIsLocal) {
    ActorId id(7);
    std::array<uint8_t, 16> loopback{};
    loopback[15] = 1; // ::1
    ActorAddress addr{Ipv6Endpoint{loopback, 0}, 2, id, 0};
    EXPECT_TRUE(addr.is_local());
}

TEST(ActorAddressBranchesTest, Ipv6NonLoopbackIsRemote) {
    ActorId id(7);
    std::array<uint8_t, 16> addr_bytes{};
    addr_bytes[0] = 0x20; // 2001::1
    addr_bytes[1] = 0x01;
    addr_bytes[15] = 1;
    ActorAddress addr{Ipv6Endpoint{addr_bytes, 0}, 2, id, 0};
    EXPECT_FALSE(addr.is_local());
}

TEST(ActorAddressBranchesTest, ToString) {
    ActorId id(99);
    ActorAddress addr{LocalEndpoint, 1, id, 0};
    auto s = addr.to_string();
    EXPECT_FALSE(s.empty());
}

TEST(ActorAddressBranchesTest, EqualityWithDifferentTypes) {
    ActorId id(1);
    ActorAddress a{LocalEndpoint, 0, id, 0};
    ActorAddress b{LocalEndpoint, 1, id, 0}; // different type
    EXPECT_NE(a, b);
}

TEST(ActorAddressBranchesTest, EqualityWithDifferentEndpoints) {
    ActorId id(1);
    ActorAddress a{LocalEndpoint, 0, id, 0};
    Ipv4Endpoint ep2{0x7F000002, 0}; // 127.0.0.2
    ActorAddress b{ep2, 0, id, 0};
    EXPECT_NE(a, b);
}

TEST(ActorAddressBranchesTest, InvalidSentinel) {
    ActorAddr invalid = invalid_actor_addr;
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.id.value(), 0U);
    EXPECT_EQ(invalid.type, 0U);
    EXPECT_EQ(invalid.incarnation, 0U);
}

TEST(ActorAddressBranchesTest, HashDifferentIncarnations) {
    ActorId id(1);
    ActorAddress a{LocalEndpoint, 0, id, 0};
    ActorAddress b{LocalEndpoint, 0, id, 1};
    std::hash<ActorAddress> hasher;
    EXPECT_NE(hasher(a), hasher(b));
}

// ============================================================================
// ActorProxy — remote actor reference, send routing
// ============================================================================

TEST(ActorProxyTest, ConstructWithAddressAndNullTransport) {
    ActorId id(100);
    ActorAddress addr{LocalEndpoint, 1, id, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));

    EXPECT_FALSE(proxy.is_local());
    EXPECT_TRUE(proxy); // valid address
    EXPECT_EQ(proxy.address(), addr);
    EXPECT_EQ(proxy.transport(), nullptr);
}

TEST(ActorProxyTest, EndpointMatchesAddress) {
    ActorId id(200);
    Ipv4Endpoint ep{0xC0A8010A, 9090}; // 192.168.1.10:9090
    ActorAddress addr{ep, 1, id, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));

    auto proxy_ep = proxy.endpoint();
    // Verify endpoint was stored (matches address endpoint)
    EXPECT_EQ(proxy_ep, addr.endpoint);
}

TEST(ActorProxyTest, TrySendWithNullTransportReturnsNoRoute) {
    ActorId id(300);
    ActorAddress addr{LocalEndpoint, 1, id, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));

    // Create a minimal TypedMessage for try_send
    TypedMessage msg(TypeTag::User, StreamBuffer{});

    auto receipt = proxy.try_send(addr, std::move(msg));
    // With null transport, should return NoRoute (dead-letter path)
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, mailbox::DeliveryStatus::NoRoute);
    EXPECT_FALSE(receipt.get().ok());
}

TEST(ActorProxyTest, SetDiscoveryAndLocationCache) {
    ActorId id(400);
    ActorAddress addr{LocalEndpoint, 1, id, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));

    EXPECT_EQ(proxy.discovery_, nullptr);
    EXPECT_EQ(proxy.location_cache_, nullptr);

    net::IServiceDiscovery* sd = nullptr;
    proxy.set_discovery(sd);
    EXPECT_EQ(proxy.discovery_, sd);

    net::ActorLocationCache* lc = nullptr;
    proxy.set_location_cache(lc);
    EXPECT_EQ(proxy.location_cache_, lc);
}

// ============================================================================
// ActorRef — variant dispatcher (local vs remote)
// ============================================================================

TEST(ActorRefTest, DefaultConstructedIsInvalid) {
    ActorRef ref;
    EXPECT_FALSE(ref);
    EXPECT_TRUE(ref.is_local()); // default variant holds Actor
    // get_actor() returns address of variant's Actor (not nullptr),
    // but the underlying Actor is empty (evaluates to false)
    EXPECT_NE(ref.get_actor(), nullptr); // points to variant storage
    EXPECT_FALSE(*ref.get_actor());      // but the Actor itself is empty
    EXPECT_EQ(ref.get_proxy(), nullptr);
}

TEST(ActorRefTest, ConstructFromLocalActor) {
    Actor a; // empty actor
    ActorRef ref(a);
    EXPECT_TRUE(ref.is_local());
    EXPECT_FALSE(ref); // underlying actor is empty
    EXPECT_NE(ref.get_actor(), nullptr);
    EXPECT_EQ(ref.get_proxy(), nullptr);
}

TEST(ActorRefTest, ConstructFromActorProxy) {
    ActorId id(500);
    ActorAddress addr{LocalEndpoint, 1, id, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));

    ActorRef ref(proxy);
    EXPECT_FALSE(ref.is_local());
    EXPECT_TRUE(ref); // valid proxy
    EXPECT_EQ(ref.get_actor(), nullptr);
    EXPECT_NE(ref.get_proxy(), nullptr);
}

TEST(ActorRefTest, AddressForwardsToVariant) {
    ActorId id(600);
    ActorAddress addr{LocalEndpoint, 1, id, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));

    ActorRef ref(proxy);
    EXPECT_EQ(ref.address(), addr);
    EXPECT_EQ(ref.endpoint(), addr.endpoint);
}

TEST(ActorRefTest, ProxyTrySendReturnsNoRouteOnNullTransport) {
    // Construct a valid ActorProxy with null transport and verify
    // try_send returns NoRoute (dead-letter path).
    ActorId id(700);
    ActorAddress addr{LocalEndpoint, 1, id, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorRef ref(proxy);

    EXPECT_FALSE(ref.is_local());
    ASSERT_NE(ref.get_proxy(), nullptr);

    TypedMessage msg(TypeTag::User, StreamBuffer(1, static_cast<uint8_t>(0)));
    auto receipt = ref.try_send(addr, std::move(msg));
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, mailbox::DeliveryStatus::NoRoute);
    EXPECT_FALSE(receipt.get().ok());
}

// ============================================================================
// ActorRefCache — LRU caching for actor references
// ============================================================================

TEST(ActorRefCacheTest, GetOnEmptyCacheReturnsNullopt) {
    ActorRefCache cache(16);
    auto result = cache.get(ActorId{1});
    EXPECT_FALSE(result.has_value());
}

TEST(ActorRefCacheTest, PutAndGetRoundTrip) {
    ActorRefCache cache(8);

    ActorId id{42};
    Actor a;
    ActorRef ref(a);
    cache.put(id, ref);

    auto result = cache.get(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->is_local(), ref.is_local());
}

TEST(ActorRefCacheTest, PutOverwriteExisting) {
    ActorRefCache cache(8);
    ActorId id{99};

    Actor a1;
    ActorRef ref1(a1);
    cache.put(id, ref1);

    ActorProxy proxy(ActorAddress{LocalEndpoint, 1, ActorId{99}, 0},
                     static_cast<net::Transport*>(nullptr));
    ActorRef ref2(proxy);
    cache.put(id, ref2);

    auto result = cache.get(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_local()); // should be the proxy now
}

TEST(ActorRefCacheTest, EvictsLRUWhenFull) {
    ActorRefCache cache(2); // small cache

    Actor a;
    cache.put(ActorId{1}, ActorRef(a));
    cache.put(ActorId{2}, ActorRef(a));

    // Access id=1 to make id=2 LRU
    auto r1 = cache.get(ActorId{1});
    EXPECT_TRUE(r1.has_value());

    // Insert id=3 — should evict id=2 (LRU)
    cache.put(ActorId{3}, ActorRef(a));

    auto r2 = cache.get(ActorId{2});
    EXPECT_FALSE(r2.has_value()); // evicted

    auto r3 = cache.get(ActorId{3});
    EXPECT_TRUE(r3.has_value()); // present
}

TEST(ActorRefCacheTest, DefaultMaxEntries) {
    ActorRefCache cache; // default 256 entries
    // Should accept many entries without issue
    for (uint64_t i = 1; i <= 100; ++i) {
        cache.put(ActorId{i}, ActorRef(Actor{}));
    }
    auto result = cache.get(ActorId{50});
    EXPECT_TRUE(result.has_value());
}

// ============================================================================
// ActorLocationCache — TTL-based location cache
// ============================================================================

TEST(ActorLocationCacheTest, GetOnEmptyReturnsNullopt) {
    net::ActorLocationCache cache;
    auto result = cache.get(ActorId{1});
    EXPECT_FALSE(result.has_value());
}

TEST(ActorLocationCacheTest, PutAndGetRoundTrip) {
    net::ActorLocationCache cache;
    ActorId id{77};
    Ipv4Endpoint ep{0xC0A80101, htons(8080)}; // 192.168.1.1:8080
    EndPoint endpoint{ep};

    cache.put(id, endpoint);

    auto result = cache.get(id);
    ASSERT_TRUE(result.has_value());
    auto* ipv4 = std::get_if<Ipv4Endpoint>(&*result);
    ASSERT_NE(ipv4, nullptr);
    EXPECT_EQ(ipv4->port(), 8080U);
}

TEST(ActorLocationCacheTest, EvictRemovesEntry) {
    net::ActorLocationCache cache;
    ActorId id{88};
    cache.put(id, EndPoint{Ipv4Endpoint{0x7F000001, 0}});

    auto result = cache.get(id);
    EXPECT_TRUE(result.has_value());

    cache.evict(id);
    auto result2 = cache.get(id);
    EXPECT_FALSE(result2.has_value());
}

TEST(ActorLocationCacheTest, EvictNodeRemovesAllForEndpoint) {
    net::ActorLocationCache cache;
    Ipv4Endpoint ep1{0xC0A80101, 8080};
    EndPoint endpoint{ep1};

    cache.put(ActorId{1}, endpoint);
    cache.put(ActorId{2}, endpoint);
    cache.put(ActorId{3}, EndPoint{Ipv4Endpoint{0xC0A80102, 8080}});

    cache.evict_node(endpoint);

    EXPECT_FALSE(cache.get(ActorId{1}).has_value());
    EXPECT_FALSE(cache.get(ActorId{2}).has_value());
    EXPECT_TRUE(cache.get(ActorId{3}).has_value()); // different endpoint
}

TEST(ActorLocationCacheTest, PurgeExpiredClearsStaleEntries) {
    net::ActorLocationCache cache;
    // Put with 0-second TTL — should expire immediately
    cache.put(ActorId{55}, EndPoint{Ipv4Endpoint{0x7F000001, 0}},
              std::chrono::seconds(0));

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cache.purge_expired();
    auto result = cache.get(ActorId{55});
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// DeliveryResult — status codes and helper methods
// ============================================================================

TEST(DeliveryResultTest, DefaultConstruction) {
    mailbox::DeliveryResult result;
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::Accepted);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.accepted());
}

TEST(DeliveryResultTest, OkWhenAcceptedWithPressure) {
    mailbox::DeliveryResult result;
    result.status = mailbox::DeliveryStatus::AcceptedWithPressure;
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.accepted());
}

TEST(DeliveryResultTest, NotOkWhenNoRoute) {
    mailbox::DeliveryResult result;
    result.status = mailbox::DeliveryStatus::NoRoute;
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.accepted());
}

TEST(DeliveryResultTest, RetryableStatuses) {
    mailbox::DeliveryResult result;

    result.status = mailbox::DeliveryStatus::MailboxFull;
    EXPECT_TRUE(result.retryable());

    result.status = mailbox::DeliveryStatus::NoRoute;
    EXPECT_TRUE(result.retryable());

    result.status = mailbox::DeliveryStatus::ActorDead;
    EXPECT_TRUE(result.retryable());

    // Accepted is NOT retryable
    result.status = mailbox::DeliveryStatus::Accepted;
    EXPECT_FALSE(result.retryable());

    // Expired is NOT retryable
    result.status = mailbox::DeliveryStatus::Expired;
    EXPECT_FALSE(result.retryable());
}

TEST(DeliveryResultTest, FromEnqueueMapsStatus) {
    // Test the static factory method with an enqueue result
    mailbox::EnqueueResult er;
    er.code = mailbox::EnqueueResultCode::Accepted;
    auto result = mailbox::DeliveryResult::from_enqueue(
        er, ActorAddress{LocalEndpoint, 0, ActorId{1}, 0});
    EXPECT_TRUE(result.ok());
}

TEST(DeliveryResultTest, FromTransportMapsStatus) {
    auto result = mailbox::DeliveryResult::from_transport(
        TransportSendResult::Sent, ActorAddress{}, MessageId{});
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.accepted());
}
