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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/ref/actor_address.hpp>

#include "runtime/messaging_network_emitters.hpp"

namespace hpactor {
namespace {

// ── Test fixture ──────────────────────────────────────────────────────────

class MessagingNetworkEmittersTest : public ::testing::Test {
  protected:
    void SetUp() override {
        captured_target_ = ActorAddress{};
        captured_acker_ = ActorAddress{};
        captured_msg_id_ = 0;
        captured_status_ = 0;
        captured_retry_ms_ = 0;
        captured_encoded_.clear();
        ack_call_count_ = 0;
        bp_call_count_ = 0;
    }

    // ── ReliableAckEmitter test adapter ─────────────────────────────────
    static void test_ack_emit(void* context, const ActorAddress& target,
                              const ActorAddress& acker, uint64_t message_id,
                              uint8_t status, uint32_t retry_after_ms) noexcept {
        auto* self = static_cast<MessagingNetworkEmittersTest*>(context);
        self->captured_target_ = target;
        self->captured_acker_ = acker;
        self->captured_msg_id_ = message_id;
        self->captured_status_ = status;
        self->captured_retry_ms_ = retry_after_ms;
        ++self->ack_call_count_;
    }

    // ── BackpressureSignalEmitter test adapter ────────────────────────────
    static bool test_bp_send(void* context, const ActorAddress& target,
                             const StreamBuffer& encoded) noexcept {
        auto* self = static_cast<MessagingNetworkEmittersTest*>(context);
        self->captured_target_ = target;
        self->captured_encoded_ = std::string(encoded.begin(), encoded.end());
        ++self->bp_call_count_;
        return true;
    }

    ActorAddress captured_target_;
    ActorAddress captured_acker_;
    uint64_t captured_msg_id_;
    uint8_t captured_status_;
    uint32_t captured_retry_ms_;
    std::string captured_encoded_;
    int ack_call_count_;
    int bp_call_count_;
};

// ── ReliableAckEmitter tests ─────────────────────────────────────────────────

TEST_F(MessagingNetworkEmittersTest, ReliableAckEmitterNullContextIsSafeNoop) {
    ReliableAckEmitter port;
    port.context = nullptr;
    port.emit = test_ack_emit;
    // operator() should safely return when context is null
    port(ActorAddress{}, ActorAddress{}, 42, 0, 0);
    EXPECT_EQ(ack_call_count_, 0);
}

TEST_F(MessagingNetworkEmittersTest, ReliableAckEmitterNullEmitIsSafeNoop) {
    ReliableAckEmitter port;
    port.context = this;
    port.emit = nullptr;
    // operator() should safely return when emit is null
    port(ActorAddress{}, ActorAddress{}, 42, 0, 0);
    EXPECT_EQ(ack_call_count_, 0);
}

TEST_F(MessagingNetworkEmittersTest, ReliableAckEmitterForwardsArgumentsExactlyOnce) {
    ActorAddress target;
    target.id = ActorId{10};
    ActorAddress acker;
    acker.id = ActorId{20};

    ReliableAckEmitter port;
    port.context = this;
    port.emit = test_ack_emit;
    port(target, acker, 12345, 2, 5000);

    EXPECT_EQ(ack_call_count_, 1);
    EXPECT_EQ(captured_target_.id, ActorId{10});
    EXPECT_EQ(captured_acker_.id, ActorId{20});
    EXPECT_EQ(captured_msg_id_, 12345u);
    EXPECT_EQ(captured_status_, 2u);
    EXPECT_EQ(captured_retry_ms_, 5000u);
}

// ── BackpressureSignalEmitter tests ────────────────────────────────────────────

TEST_F(MessagingNetworkEmittersTest, BackpressureSignalEmitterNullContextReturnsFalse) {
    BackpressureSignalEmitter port;
    port.context = nullptr;
    port.send = test_bp_send;
    bool result = port(ActorAddress{}, StreamBuffer{});
    EXPECT_FALSE(result);
    EXPECT_EQ(bp_call_count_, 0);
}

TEST_F(MessagingNetworkEmittersTest, BackpressureSignalEmitterNullSendReturnsFalse) {
    BackpressureSignalEmitter port;
    port.context = this;
    port.send = nullptr;
    bool result = port(ActorAddress{}, StreamBuffer{});
    EXPECT_FALSE(result);
    EXPECT_EQ(bp_call_count_, 0);
}

TEST_F(MessagingNetworkEmittersTest, BackpressureSignalEmitterForwardsExactlyOnce) {
    ActorAddress target;
    target.id = ActorId{30};
    const char hello[] = "hello";
    auto data = StreamBuffer::from_data(reinterpret_cast<const uint8_t*>(hello), 5);

    BackpressureSignalEmitter port;
    port.context = this;
    port.send = test_bp_send;
    bool result = port(target, std::move(data));

    EXPECT_TRUE(result);
    EXPECT_EQ(bp_call_count_, 1);
    EXPECT_EQ(captured_target_.id, ActorId{30});
    EXPECT_EQ(captured_encoded_, "hello");
}

// ── MessagingNetworkEmitters aggregate tests ─────────────────────────────────

TEST_F(MessagingNetworkEmittersTest, DefaultPortsAreSafeNoops) {
    MessagingNetworkEmitters ports;
    // Default-constructed ports have null context/emit/send
    ports.reliable_ack(ActorAddress{}, ActorAddress{}, 1, 0, 0);
    // Should not crash
    bool result = ports.backpressure(ActorAddress{}, StreamBuffer{});
    EXPECT_FALSE(result);
}

TEST_F(MessagingNetworkEmittersTest, PortsAreTriviallyCopyable) {
    // Port structs contain only function pointers and void* — no heap
    // allocation
    EXPECT_TRUE(std::is_trivially_copyable_v<ReliableAckEmitter>);
    EXPECT_TRUE(std::is_trivially_copyable_v<BackpressureSignalEmitter>);
    EXPECT_TRUE(std::is_trivially_copyable_v<MessagingNetworkEmitters>);
}

} // namespace
} // namespace hpactor
