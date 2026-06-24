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

#include <hpactor/msg/frame.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/tracing/span.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;

namespace {

class MockTransport : public hpactor::net::Transport {
  public:
    TransportSendResult try_send(const hpactor::ActorAddress&,
                                 const hpactor::StreamBuffer& encoded) override {
        sent_frames_.push_back(encoded);
        return TransportSendResult::Sent;
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

class RpcChannelBranchesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        channel_ = std::make_unique<RpcChannel>(&transport_, &scheduler_);
    }

    MockTransport transport_;
    MockScheduler scheduler_;
    std::unique_ptr<RpcChannel> channel_;
};

// Test 1: call_raw with custom max_retries via constructor
TEST_F(RpcChannelBranchesTest, CallRawWithCustomMaxRetries) {
    // Create a channel with non-default max_retries
    auto custom_channel = std::make_unique<RpcChannel>(&transport_, &scheduler_, 7);
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};

    auto future = custom_channel->call_raw(target, StreamBuffer{1, 2, 3},
                                           std::chrono::milliseconds{100});

    ASSERT_EQ(transport_.sent_frames_.size(), 1u);
    EXPECT_FALSE(future.ready());

    // Decode frame to get the actual message ID
    net::WireFrame frame = net::WireFrame::decode(transport_.sent_frames_[0]);
    MessageId actual_msg_id =
        MessageId(frame.pb_envelope.data_frame().message_id());

    // Simulate response
    StreamBuffer response_data = {10, 20, 30};
    RpcResponseFrame response_frame;
    response_frame.msg_id = actual_msg_id;
    response_frame.payload = response_data;
    custom_channel->on_response(response_frame);

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), response_data);
}

// Test 2: call_raw with zero timeout — verifies timeout path for extremely
//         short timeout values
TEST_F(RpcChannelBranchesTest, CallRawWithVeryShortTimeout) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};
    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{0});

    auto result = future.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), errors::timeout);
}

// Test 3: RpcFuture::ready() on unresolved future
TEST_F(RpcChannelBranchesTest, FutureReadyUnresolved) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};
    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{5000});

    EXPECT_FALSE(future.ready());
}

// Test 4: RpcFuture::ready() after response arrives
TEST_F(RpcChannelBranchesTest, FutureReadyAfterResponse) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};
    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{5000});

    net::WireFrame frame = net::WireFrame::decode(transport_.sent_frames_[0]);
    MessageId actual_msg_id =
        MessageId(frame.pb_envelope.data_frame().message_id());

    // Before response — not ready
    EXPECT_FALSE(future.ready());

    RpcResponseFrame response_frame;
    response_frame.msg_id = actual_msg_id;
    response_frame.payload = StreamBuffer{4, 5, 6};
    channel_->on_response(response_frame);

    // After response — ready
    EXPECT_TRUE(future.ready());
}

// Test 5: RpcFuture::get() after response already delivered
TEST_F(RpcChannelBranchesTest, FutureGetAfterResponse) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};
    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{5000});

    net::WireFrame frame = net::WireFrame::decode(transport_.sent_frames_[0]);
    MessageId actual_msg_id =
        MessageId(frame.pb_envelope.data_frame().message_id());

    RpcResponseFrame response_frame;
    response_frame.msg_id = actual_msg_id;
    response_frame.payload = StreamBuffer{7, 8, 9};
    channel_->on_response(response_frame);

    // get() should return immediately since promise is resolved
    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), (StreamBuffer{7, 8, 9}));
}

// Test 6: abort() during in-flight calls — all futures must resolve with error
TEST_F(RpcChannelBranchesTest, AbortDuringPendingCalls) {
    std::vector<RpcFuture<StreamBuffer>> futures;
    for (int i = 0; i < 5; i++) {
        ActorAddress target{LocalEndpoint, 1,
                            ActorId{10u + static_cast<uint64_t>(i)}, 0};
        futures.push_back(
            channel_->call_raw(target, StreamBuffer{static_cast<uint8_t>(i)},
                               std::chrono::milliseconds{5000}));
    }
    ASSERT_EQ(futures.size(), 5u);
    ASSERT_EQ(transport_.sent_frames_.size(), 5u);

    channel_->abort();

    for (auto& future : futures) {
        EXPECT_TRUE(future.ready());
        auto result = future.get();
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), errors::unknown);
    }
}

// Test 7: abort() with no pending calls — should be a no-op / not crash
TEST_F(RpcChannelBranchesTest, AbortEmptyChannel) {
    channel_->abort();
    // Channel should still be usable after abort of empty pending map
    ActorAddress target{LocalEndpoint, 1, ActorId{99}, 0};
    auto future = channel_->call_raw(target, StreamBuffer{42},
                                     std::chrono::milliseconds{1000});
    ASSERT_EQ(transport_.sent_frames_.size(), 1u);
    EXPECT_FALSE(future.ready());
}

// Test 8: on_response with unknown msg_id — silently dropped
TEST_F(RpcChannelBranchesTest, OnResponseUnknownMsgId) {
    // Send one real call to have something in the map
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};
    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{5000});

    // Now simulate response with a completely unrelated msg_id
    RpcResponseFrame unknown_response;
    unknown_response.msg_id = MessageId{999999};
    unknown_response.payload = StreamBuffer{99};

    // Should not crash, and the original future should still be pending
    channel_->on_response(unknown_response);
    EXPECT_FALSE(future.ready());
}

// Test 9: trace-aware call_raw
TEST_F(RpcChannelBranchesTest, TraceAwareCallRaw) {
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};

    TraceContext parent_ctx;
    parent_ctx.trace_id.bytes[0] = 0xAB;
    parent_ctx.trace_id.bytes[1] = 0xCD;
    parent_ctx.span_id.bytes[0] = 0x12;
    parent_ctx.span_id.bytes[1] = 0x34;
    parent_ctx.flags.set_sampled(true);

    auto future = channel_->call_raw(target, StreamBuffer{1, 2, 3},
                                     std::chrono::milliseconds{1000}, &parent_ctx);

    ASSERT_EQ(transport_.sent_frames_.size(), 1u);

    // Verify the frame was sent (trace context is embedded in the wire frame)
    net::WireFrame frame = net::WireFrame::decode(transport_.sent_frames_[0]);
    MessageId actual_msg_id =
        MessageId(frame.pb_envelope.data_frame().message_id());

    // Respond to complete the call
    RpcResponseFrame response_frame;
    response_frame.msg_id = actual_msg_id;
    response_frame.payload = StreamBuffer{4, 5, 6};
    channel_->on_response(response_frame);

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), (StreamBuffer{4, 5, 6}));
}

// Test 10: multiple concurrent calls with individual responses
TEST_F(RpcChannelBranchesTest, ConcurrentWithPerCallResponses) {
    constexpr int kNumCalls = 5;
    std::vector<RpcFuture<StreamBuffer>> futures;
    std::vector<MessageId> msg_ids;

    for (int i = 0; i < kNumCalls; i++) {
        ActorAddress target{
            LocalEndpoint, 1,
            ActorId{static_cast<uint64_t>(100u + static_cast<uint64_t>(i))}, 0};
        futures.push_back(
            channel_->call_raw(target, StreamBuffer{static_cast<uint8_t>(i)},
                               std::chrono::milliseconds{5000}));
    }

    ASSERT_EQ(transport_.sent_frames_.size(), static_cast<size_t>(kNumCalls));

    // Extract the actual MessageIds from the wire frames
    for (size_t i = 0; i < transport_.sent_frames_.size(); i++) {
        net::WireFrame frame = net::WireFrame::decode(transport_.sent_frames_[i]);
        msg_ids.push_back(MessageId(frame.pb_envelope.data_frame().message_id()));
    }

    // Respond to each call with the correct msg_id
    for (size_t i = 0; i < static_cast<size_t>(kNumCalls); i++) {
        RpcResponseFrame response_frame;
        response_frame.msg_id = msg_ids[i];
        response_frame.payload = StreamBuffer{static_cast<uint8_t>(i + 100)};
        channel_->on_response(response_frame);
    }

    // All futures should be ready with correct payloads
    for (size_t i = 0; i < static_cast<size_t>(kNumCalls); i++) {
        EXPECT_TRUE(futures[i].ready());
        auto result = futures[i].get();
        ASSERT_TRUE(result.has_value());
        auto expected_payload = StreamBuffer{static_cast<uint8_t>(i + 100)};
        EXPECT_EQ(result.value(), expected_payload);
    }
}

// Test 11: retry path with max_retries exhausted leads to deadline error
TEST_F(RpcChannelBranchesTest, DeadlineExpiryAfterMaxRetries) {
    // Create channel with max_retries = 1 (so 2 total attempts)
    auto low_retry_channel =
        std::make_unique<RpcChannel>(&transport_, &scheduler_, 1);
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};

    auto future = low_retry_channel->call_raw(target, StreamBuffer{1, 2, 3},
                                              std::chrono::milliseconds{100});

    // First send
    EXPECT_EQ(transport_.sent_frames_.size(), 1u);

    // First timeout — retry (#1, with retry_count 0)
    scheduler_.invoke_timer(1);
    // Second send via retry
    EXPECT_GE(transport_.sent_frames_.size(), 2u);

    // Second timeout — this should exhaust retries (retry_count==1 >=
    // max_retries==1)
    scheduler_.invoke_timer(2);

    auto result = future.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), errors::timeout);
}

// Test 12: RpcChannel with different default_max_retries affects deadline
TEST_F(RpcChannelBranchesTest, ChannelWithZeroMaxRetries) {
    auto no_retry_channel =
        std::make_unique<RpcChannel>(&transport_, &scheduler_, 0);
    ActorAddress target{LocalEndpoint, 1, ActorId{1}, 0};

    auto future = no_retry_channel->call_raw(target, StreamBuffer{1, 2, 3},
                                             std::chrono::milliseconds{100});

    EXPECT_EQ(transport_.sent_frames_.size(), 1u);

    // First timeout — no retries configured, should fail immediately
    scheduler_.invoke_timer(1);

    auto result = future.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), errors::timeout);
}
