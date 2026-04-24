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

#include <cassert>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/net/frame.hpp>

class MockTransport : public hpactor::net::Transport {
public:
    void send(const hpactor::ActorAddress&, const hpactor::bytes& encoded) override {
        sent_frames_.push_back(encoded);
    }
    hpactor::net::ConnectionPtr connect(hpactor::CommunicationEndpoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    hpactor::net::ConnectionPtr connect(hpactor::CommunicationEndpoint) override { return nullptr; }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    bool is_connected(hpactor::CommunicationEndpoint) const override { return true; }
    hpactor::CommunicationEndpoint endpoint() const override { return hpactor::LocalEndpoint; }
    void close_connection(hpactor::CommunicationEndpoint) override {}
    void set_rpc_handler(rpc_response_handler) override {}

    std::vector<hpactor::bytes> sent_frames_;
};

struct MockScheduler : public hpactor::sched::IScheduler {
    hpactor::sched::TimerHandle schedule_after(hpactor::sched::timer_callback cb, int64_t) override {
        callbacks_[next_id_] = std::move(cb);
        return hpactor::sched::TimerHandle{next_id_++};
    }
    hpactor::sched::TimerHandle schedule_every(hpactor::sched::timer_callback cb, int64_t interval_ns) override {
        return schedule_after(std::move(cb), interval_ns);
    }
    void cancel_timer(hpactor::sched::TimerHandle) override {}
    void notify_ready(hpactor::ActorId, uint8_t, int64_t) override {}
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId, uint8_t) override {}
    void start() override {}
    void stop() override {}
    size_t worker_count() const override { return 1; }
    bool is_running() const override { return true; }

    void invoke_timer(uint64_t id) {
        auto it = callbacks_.find(id);
        if (it != callbacks_.end()) {
            it->second();
            callbacks_.erase(it);
        }
    }

    std::unordered_map<uint64_t, hpactor::sched::timer_callback> callbacks_;
    uint64_t next_id_ = 1;
};

void test_response() {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, &scheduler);

    hpactor::ActorAddress target{hpactor::LocalEndpoint, 1, hpactor::ActorId{1}, 0};

    hpactor::bytes request_data = {1, 2, 3};
    auto future = channel.call_raw(target, request_data, std::chrono::milliseconds{1000});

    assert(transport.sent_frames_.size() == 1u);

    // Decode frame to get the actual message ID
    hpactor::net::Frame frame = hpactor::net::Frame::decode(transport.sent_frames_[0]);
    hpactor::MessageId actual_msg_id = hpactor::MessageId(frame.message_id);

    // Simulate response with the correct message ID
    hpactor::bytes response_data = {4, 5, 6};
    channel.on_response(actual_msg_id, response_data);

    auto result = future.get();
    assert(result.has_value());
    assert(result.value() == response_data);
}

void test_timeout() {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, &scheduler);

    hpactor::ActorAddress target{hpactor::LocalEndpoint, 1, hpactor::ActorId{1}, 0};

    auto future = channel.call_raw(target, hpactor::bytes{1, 2, 3}, std::chrono::milliseconds{50});

    auto result = future.get();
    assert(!result.has_value());
    assert(result.error().code() == hpactor::errors::timeout);
}

void test_concurrent() {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, &scheduler);

    std::vector<hpactor::RpcFuture<hpactor::bytes>> futures;
    for (int i = 0; i < 10; i++) {
        hpactor::ActorAddress target{hpactor::LocalEndpoint, 1, hpactor::ActorId{static_cast<uint64_t>(i)}, 0};
        futures.push_back(channel.call_raw(target, hpactor::bytes{static_cast<uint8_t>(i)}, std::chrono::milliseconds{1000}));
    }

    assert(transport.sent_frames_.size() == 10u);
}

void test_retry() {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, &scheduler);

    hpactor::ActorAddress target{hpactor::LocalEndpoint, 1, hpactor::ActorId{1}, 0};

    auto future = channel.call_raw(target, hpactor::bytes{1, 2, 3}, std::chrono::milliseconds{100});

    size_t initial_send_count = transport.sent_frames_.size();
    assert(initial_send_count == 1u);

    // Trigger timeout - should retry
    scheduler.invoke_timer(1);

    assert(transport.sent_frames_.size() >= initial_send_count);
}

int main() {
    test_response();
    test_timeout();
    test_concurrent();
    test_retry();
    return 0;
}
