#include <cassert>
#include <cstdio>
#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/transport.hpp>

using namespace hpactor;

namespace {
struct NullTransport : public net::Transport {
    bool try_send(const ActorAddress&, const StreamBuffer&) override {
        return true;
    }
    void send(const ActorAddress&, const StreamBuffer&) override {}
    net::ConnectionPtr connect(EndPoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    net::ConnectionPtr connect(EndPoint) override {
        return nullptr;
    }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    bool is_connected(EndPoint) const override {
        return false;
    }
    EndPoint endpoint() const override {
        return {};
    }
    void close_connection(EndPoint) override {}
    void set_rpc_handler(rpc_response_handler) override {}
};
} // namespace

void test_construct_and_make_behavior() {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);
    ActorTypeRegistry registry;
    NullTransport transport;
    SpawnReceiver receiver(sys, registry, &transport);
    auto behavior = receiver.make_behavior();
    (void)behavior;
    printf("  PASSED test_construct_and_make_behavior\n");
}

void test_construct_without_transport() {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);
    ActorTypeRegistry registry;
    SpawnReceiver receiver(sys, registry, nullptr);
    auto behavior = receiver.make_behavior();
    (void)behavior;
    printf("  PASSED test_construct_without_transport\n");
}

int main() {
    printf("SpawnReceiver tests:\n");
    test_construct_and_make_behavior();
    test_construct_without_transport();
    printf("All SpawnReceiver tests PASSED\n");
    return 0;
}
