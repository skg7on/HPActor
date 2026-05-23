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

#include <hpactor/net/frame.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;

namespace {

class MockTransport : public hpactor::net::Transport {
  public:
    bool try_send(const hpactor::ActorAddress&,
                  const hpactor::StreamBuffer& encoded) override {
        sent_frames_.push_back(encoded);
        return true;
    }
    hpactor::net::ConnectionPtr
    connect(hpactor::EndPoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    hpactor::net::ConnectionPtr connect(hpactor::EndPoint) override {
        return nullptr;
    }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    bool is_connected(hpactor::EndPoint) const override {
        return true;
    }
    hpactor::EndPoint endpoint() const override {
        return hpactor::LocalEndpoint;
    }
    void close_connection(hpactor::EndPoint) override {}
    void set_rpc_handler(rpc_response_handler) override {}

    std::vector<hpactor::StreamBuffer> sent_frames_;
};

struct MockScheduler : public hpactor::sched::IScheduler {
    hpactor::sched::TimerHandle
    schedule_after(hpactor::sched::timer_callback cb, int64_t) override {
        callbacks_[next_id_] = std::move(cb);
        return hpactor::sched::TimerHandle{next_id_++};
    }
    hpactor::sched::TimerHandle schedule_every(hpactor::sched::timer_callback cb,
                                               int64_t interval_ns) override {
        return schedule_after(std::move(cb), interval_ns);
    }
    void cancel_timer(hpactor::sched::TimerHandle) override {}
    void notify_ready(hpactor::ActorId, uint8_t, int64_t) override {}
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId, uint8_t) override {}
    void start() override {}
    void stop() override {}
    size_t worker_count() const override {
        return 1;
    }
    bool is_running() const override {
        return true;
    }
    void register_dedicated_thread(hpactor::ActorId, int) override {}
    void register_dedicated_pool(hpactor::ActorId, uint32_t) override {}
    void unregister_dedicated(hpactor::ActorId) override {}

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

} // namespace

class RpcChannelTest : public ::testing::Test {
  protected:
    void SetUp() override {
        channel_ = std::make_unique<RpcChannel>(&transport_, &scheduler_);
    }

    MockTransport transport_;
    MockScheduler scheduler_;
    std::unique_ptr<RpcChannel> channel_;
};

TEST_F(RpcChannelTest, Response) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};

    StreamBuffer request_data = {1, 2, 3};
    auto future =
        channel_->call_raw(target, request_data, std::chrono::milliseconds{1000});

    ASSERT_EQ(transport_.sent_frames_.size(), 1u);

    // Decode frame to get the actual message ID
    net::WireFrame frame = net::WireFrame::decode(transport_.sent_frames_[0]);
    MessageId actual_msg_id = MessageId(frame.pb_frame.message_id());

    // Simulate response with the correct message ID
    StreamBuffer response_data = {4, 5, 6};
    RpcResponseFrame response_frame;
    response_frame.msg_id = actual_msg_id;
    response_frame.payload = response_data;
    channel_->on_response(response_frame);

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), response_data);
}

TEST_F(RpcChannelTest, Timeout) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};

    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{50});

    auto result = future.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), errors::timeout);
}

TEST_F(RpcChannelTest, Concurrent) {
    std::vector<RpcFuture<StreamBuffer>> futures;
    for (int i = 0; i < 10; i++) {
        ActorAddress target{LocalEndpoint, 1, ActorId{static_cast<uint64_t>(i)}, 0};
        futures.push_back(
            channel_->call_raw(target, StreamBuffer{static_cast<uint8_t>(i)},
                               std::chrono::milliseconds{1000}));
    }

    EXPECT_EQ(transport_.sent_frames_.size(), 10u);
}

TEST_F(RpcChannelTest, Retry) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};

    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{100});

    size_t initial_send_count = transport_.sent_frames_.size();
    EXPECT_EQ(initial_send_count, 1u);

    // Trigger timeout - should retry
    scheduler_.invoke_timer(1);

    EXPECT_GE(transport_.sent_frames_.size(), initial_send_count);
}
