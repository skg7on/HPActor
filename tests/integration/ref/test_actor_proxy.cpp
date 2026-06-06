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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>

using namespace hpactor;

namespace {

struct NullTransport : public net::Transport {
    net::ConnectionPtr connect(EndPoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    net::ConnectionPtr connect(EndPoint) override {
        return nullptr;
    }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    TransportSendResult try_send(const ActorAddress&, const StreamBuffer&) override {
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

} // namespace

class ActorProxyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ep_ = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    }

    EndPoint ep_;
};

TEST_F(ActorProxyTest, DefaultConstructorInvalidAddress) {
    ActorProxy proxy(ActorAddress{}, static_cast<net::Transport*>(nullptr));
    EXPECT_FALSE(proxy);
    EXPECT_EQ(proxy.address().endpoint, endpoint_ops::parse_endpoint(""));
    EXPECT_EQ(proxy.endpoint(), endpoint_ops::parse_endpoint(""));
    EXPECT_FALSE(proxy.is_local());
}

TEST_F(ActorProxyTest, ValidRemoteAddress) {
    ActorId id(42);
    ActorAddress addr(endpoint_ops::parse_endpoint("remotehost:12345"), 0, id, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    EXPECT_TRUE(proxy);
    EXPECT_EQ(proxy.address(), addr);
    EXPECT_EQ(proxy.endpoint(), endpoint_ops::parse_endpoint("remotehost:"
                                                             "12345"));
    EXPECT_FALSE(proxy.is_local());
}

TEST_F(ActorProxyTest, ConstructWithTransport) {
    ActorAddress addr(ep_, 0, ActorId{1}, 0);
    NullTransport transport;
    ActorProxy proxy(addr, &transport);
    SUCCEED();
}

TEST_F(ActorProxyTest, ConstructWithSystem) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);
    ActorAddress addr(ep_, 0, ActorId{2}, 0);
    ActorProxy proxy(addr, &sys);
    SUCCEED();
}

TEST_F(ActorProxyTest, TrySendNoTransport) {
    ActorAddress addr(ep_, 0, ActorId{3}, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorAddress target(ep_, 0, ActorId{4}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{});
    auto result = proxy.try_send(target, std::move(msg));
    EXPECT_EQ(result.status, mailbox::DeliveryStatus::NoRoute);
}

TEST_F(ActorProxyTest, SendFireAndForget) {
    ActorAddress addr(ep_, 0, ActorId{5}, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorAddress target(ep_, 0, ActorId{6}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{});
    proxy.send(target, std::move(msg)); // must not crash
    SUCCEED();
}
