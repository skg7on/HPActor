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

#include <hpactor/runtime/messaging_network_emitters.hpp>

namespace hpactor {
namespace {

// ── Test targets (stub implementations) ────────────────────────────────────

class StubAckTarget : public ReliableAckTarget {
  public:
    void send_ack(const ActorAddress& target, const ActorAddress& acker,
                  uint64_t message_id, uint8_t status,
                  uint32_t retry_after_ms) noexcept override {
        captured_target = target;
        captured_acker = acker;
        captured_msg_id = message_id;
        captured_status = status;
        captured_retry_ms = retry_after_ms;
        ++call_count;
    }

    ActorAddress captured_target{};
    ActorAddress captured_acker{};
    uint64_t captured_msg_id{0};
    uint8_t captured_status{0};
    uint32_t captured_retry_ms{0};
    int call_count{0};
};

class StubBPSTarget : public BackpressureSignalTarget {
  public:
    bool send_signal(const ActorAddress& target,
                     const StreamBuffer& encoded) noexcept override {
        captured_target = target;
        captured_encoded = std::string(encoded.begin(), encoded.end());
        ++call_count;
        return true;
    }

    ActorAddress captured_target{};
    std::string captured_encoded{};
    int call_count{0};
};

// ── ReliableAckEmitter tests ─────────────────────────────────────────────────

TEST(ReliableAckEmitterTest, NullTargetIsSafeNoop) {
    ReliableAckEmitter port;
    port.target_ = nullptr;
    // operator() should safely return when target is null
    port(ActorAddress{}, ActorAddress{}, 42, 0, 0);
}

TEST(ReliableAckEmitterTest, ForwardsArgumentsExactlyOnce) {
    StubAckTarget target;
    ActorAddress tgt;
    tgt.id = ActorId{10};
    ActorAddress acker;
    acker.id = ActorId{20};

    ReliableAckEmitter port;
    port.target_ = &target;
    port(tgt, acker, 12345, 2, 5000);

    EXPECT_EQ(target.call_count, 1);
    EXPECT_EQ(target.captured_target.id, ActorId{10});
    EXPECT_EQ(target.captured_acker.id, ActorId{20});
    EXPECT_EQ(target.captured_msg_id, 12345u);
    EXPECT_EQ(target.captured_status, 2u);
    EXPECT_EQ(target.captured_retry_ms, 5000u);
}

// ── BackpressureSignalEmitter tests
// ────────────────────────────────────────────

TEST(BackpressureSignalEmitterTest, NullTargetReturnsFalse) {
    BackpressureSignalEmitter port;
    port.target_ = nullptr;
    bool result = port(ActorAddress{}, StreamBuffer{});
    EXPECT_FALSE(result);
}

TEST(BackpressureSignalEmitterTest, ForwardsExactlyOnce) {
    StubBPSTarget target;
    ActorAddress tgt;
    tgt.id = ActorId{30};
    const char hello[] = "hello";
    auto data = StreamBuffer::from_data(reinterpret_cast<const uint8_t*>(hello), 5);

    BackpressureSignalEmitter port;
    port.target_ = &target;
    bool result = port(tgt, std::move(data));

    EXPECT_TRUE(result);
    EXPECT_EQ(target.call_count, 1);
    EXPECT_EQ(target.captured_target.id, ActorId{30});
    EXPECT_EQ(target.captured_encoded, "hello");
}

// ── MessagingNetworkEmitters aggregate tests ─────────────────────────────────

TEST(MessagingNetworkEmittersTest, DefaultPortsAreSafeNoops) {
    MessagingNetworkEmitters ports;
    // Default-constructed ports have null targets — safe no-ops
    ports.reliable_ack(ActorAddress{}, ActorAddress{}, 1, 0, 0);
    // Should not crash
    bool result = ports.backpressure(ActorAddress{}, StreamBuffer{});
    EXPECT_FALSE(result);
}

TEST(MessagingNetworkEmittersTest, PortsAreTriviallyCopyable) {
    // Port structs contain only a raw pointer — no heap allocation
    EXPECT_TRUE(std::is_trivially_copyable_v<ReliableAckEmitter>);
    EXPECT_TRUE(std::is_trivially_copyable_v<BackpressureSignalEmitter>);
    EXPECT_TRUE(std::is_trivially_copyable_v<MessagingNetworkEmitters>);
}

} // namespace
} // namespace hpactor
