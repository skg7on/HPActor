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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <cassert>

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
    bool try_send(const ActorAddress&, const StreamBuffer&) override {
        return true;
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

void test_construct_with_transport() {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ActorAddress addr(ep, 0, ActorId{1}, 0);
    NullTransport transport;
    ActorProxy proxy(addr, &transport);
    printf("  PASSED test_construct_with_transport\n");
}

void test_construct_with_system() {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ActorAddress addr(ep, 0, ActorId{2}, 0);
    ActorProxy proxy(addr, &sys);
    printf("  PASSED test_construct_with_system\n");
}

void test_try_send_no_transport() {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ActorAddress addr(ep, 0, ActorId{3}, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorAddress target(ep, 0, ActorId{4}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{});
    auto result = proxy.try_send(target, std::move(msg));
    // no system set either, so dead letter path skipped, but returns
    // ActorNotFound
    assert(result.code == mailbox::EnqueueResultCode::ActorNotFound);
    printf("  PASSED test_try_send_no_transport\n");
}

void test_send_fire_and_forget() {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ActorAddress addr(ep, 0, ActorId{5}, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorAddress target(ep, 0, ActorId{6}, 0);
    TypedMessage msg(TypeTag::User, StreamBuffer{});
    proxy.send(target, std::move(msg)); // must not crash
    printf("  PASSED test_send_fire_and_forget\n");
}

int main() {
    // Test ActorProxy construction with invalid address
    ActorProxy proxy1(ActorAddress{}, static_cast<net::Transport*>(nullptr));
    assert(!proxy1);
    assert(proxy1.address().endpoint == endpoint_ops::parse_endpoint(""));
    assert(proxy1.endpoint() == endpoint_ops::parse_endpoint(""));
    assert(!proxy1.is_local());

    // Test ActorProxy with valid address
    ActorId id(42);
    ActorAddress addr(endpoint_ops::parse_endpoint("remotehost:12345"), 0, id,
                      0); // remote node
    ActorProxy proxy2(addr, static_cast<net::Transport*>(nullptr));
    assert(proxy2);
    assert(proxy2.address() == addr);
    assert(proxy2.endpoint() == endpoint_ops::parse_endpoint("remotehost:"
                                                             "12345"));
    assert(!proxy2.is_local());

    test_construct_with_transport();
    test_construct_with_system();
    test_try_send_no_transport();
    test_send_fire_and_forget();

    return 0;
}
