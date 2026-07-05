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

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>

using namespace hpactor;

namespace {

// Transport that always rejects sends (to test transport failure path)
struct FailingTransport : public net::Transport {
    net::ConnectionPtr connect(EndPoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    net::ConnectionPtr connect(EndPoint) override {
        return nullptr;
    }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    TransportSendResult try_send(const ActorAddress&, const StreamBuffer&) override {
        return TransportSendResult::NotConnected;
    }
    bool is_connected(EndPoint) const override {
        return false;
    }
    EndPoint endpoint() const override {
        return Ipv4Endpoint{};
    }
    void close_connection(EndPoint) override {}
    void set_rpc_handler(rpc_response_handler) override {}
};

// Transport that remembers what was sent (for success-path verification)
struct RecordingTransport : public net::Transport {
    bool send_called = false;
    ActorAddress last_receiver;
    size_t last_payload_size = 0;

    net::ConnectionPtr connect(EndPoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    net::ConnectionPtr connect(EndPoint) override {
        return nullptr;
    }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    TransportSendResult
    try_send(const ActorAddress& receiver, const StreamBuffer& data) override {
        send_called = true;
        last_receiver = receiver;
        last_payload_size = data.size();
        return TransportSendResult::Sent;
    }
    bool is_connected(EndPoint) const override {
        return false;
    }
    EndPoint endpoint() const override {
        return Ipv4Endpoint{};
    }
    void close_connection(EndPoint) override {}
    void set_rpc_handler(rpc_response_handler) override {}
};

// Discovery that returns null for everything (missing route)
struct EmptyDiscovery : public net::IServiceDiscovery {
    void start() override {}
    void stop() override {}
    const net::Member* discover(EndPoint) const override {
        return nullptr;
    }
    std::vector<net::Member> discover_all() const override {
        return {};
    }
    void announce(net::Member) override {}
    void on_member_change(net::MemberChangeCallback) override {}
    std::string backend_name() const override {
        return "empty";
    }
};

// Discovery that returns a fixed member
struct FixedDiscovery : public net::IServiceDiscovery {
    net::Member member;

    explicit FixedDiscovery(EndPoint ep) {
        member.identity.host = "test-node";
        member.identity.endpoint = ep;
    }

    void start() override {}
    void stop() override {}
    const net::Member* discover(EndPoint) const override {
        return &member;
    }
    std::vector<net::Member> discover_all() const override {
        return {member};
    }
    void announce(net::Member) override {}
    void on_member_change(net::MemberChangeCallback) override {}
    std::string backend_name() const override {
        return "fixed";
    }
};

} // anonymous namespace

class ActorProxyDiscoveryTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ep_ = endpoint_ops::parse_endpoint("127.0.0.1:9999");
        other_ep_ = endpoint_ops::parse_endpoint("127.0.0.1:8888");
    }

    EndPoint ep_;
    EndPoint other_ep_;
};

// ── Transport failure path ───────────────────────────────────────

TEST_F(ActorProxyDiscoveryTest, TransportTrySendFailureReturnsRejected) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    FailingTransport transport;
    ActorProxy proxy(addr, &transport);

    ActorAddress target(other_ep_, 0, ActorId{2}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{'p', 'a', 'y'});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::RemoteUnavailable);
}

TEST_F(ActorProxyDiscoveryTest, TransportTrySendSuccess) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    RecordingTransport transport;
    ActorProxy proxy(addr, &transport);

    ActorAddress target(other_ep_, 0, ActorId{2}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{'d', 'a', 't', 'a'});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::Accepted);
    EXPECT_TRUE(transport.send_called);
}

// ── Discovery paths ──────────────────────────────────────────────

TEST_F(ActorProxyDiscoveryTest, DiscoveryMissingRouteReturnsActorNotFound) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    RecordingTransport transport;
    ActorProxy proxy(addr, &transport);

    EmptyDiscovery discovery;
    proxy.set_discovery(&discovery);

    ActorAddress target(other_ep_, 0, ActorId{2}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{'x'});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::NoRoute);
}

TEST_F(ActorProxyDiscoveryTest, DiscoveryResolvesAndSends) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    RecordingTransport transport;
    ActorProxy proxy(addr, &transport);

    FixedDiscovery discovery(other_ep_);
    proxy.set_discovery(&discovery);

    ActorAddress target(other_ep_, 0, ActorId{2}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{'y'});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::Accepted);
    EXPECT_TRUE(transport.send_called);
}

// ── Location cache paths ─────────────────────────────────────────

TEST_F(ActorProxyDiscoveryTest, LocationCacheHitUpdatesEndpoint) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    RecordingTransport transport;
    ActorProxy proxy(addr, &transport);

    net::ActorLocationCache cache;
    cache.put(ActorId{2}, other_ep_);
    proxy.set_location_cache(&cache);

    // Send to a target whose cached endpoint differs from the address endpoint
    ActorAddress target(ep_, 0, ActorId{2}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{'z'});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::Accepted);
    EXPECT_TRUE(transport.send_called);
    EXPECT_EQ(transport.last_receiver.endpoint, other_ep_);
}

TEST_F(ActorProxyDiscoveryTest, LocationCacheMissDoesNotCrash) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    RecordingTransport transport;
    ActorProxy proxy(addr, &transport);

    net::ActorLocationCache cache;
    proxy.set_location_cache(&cache);

    ActorAddress target(ep_, 0, ActorId{99}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{'w'});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::Accepted);
}

// ── Sender address fallback ──────────────────────────────────────

TEST_F(ActorProxyDiscoveryTest, SenderAddressFallbackWhenMsgHasNoSender) {
    ActorAddress addr(ep_, 0, ActorId{10}, 0);
    RecordingTransport transport;
    ActorProxy proxy(addr, &transport);

    TypedMessage msg(TypeTag::User, StreamBuffer{'a'});
    ActorAddress target(ep_, 0, ActorId{11}, 0);
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::Accepted);
    EXPECT_EQ(transport.last_receiver.id, ActorId{11});
}

TEST_F(ActorProxyDiscoveryTest, TransportFailureNoSystemNoCrash) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    FailingTransport transport;
    ActorProxy proxy(addr, &transport);

    ActorAddress target(other_ep_, 0, ActorId{2}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{'f'});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::RemoteUnavailable);
}
