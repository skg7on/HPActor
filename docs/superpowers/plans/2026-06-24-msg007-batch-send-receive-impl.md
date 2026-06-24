# MSG-007 Batch Send & Receive Protocol — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add batch send/receive wire protocol so N messages to the same remote target travel as one compact frame and land as one batch enqueue in the receiver's mailbox.

**Architecture:** Extend the protobuf `WireEnvelope` with a `BatchMsgFrame` oneof field, migrate `WireFrame` from raw `ActorMsgFrame` to `WireEnvelope` wrapper, add `send_batch()` APIs on `ActorContext`/`ActorProxy`/`Transport`, and dispatch incoming batches through `deliver_remote_batch()` → `try_push_batch()` on the target mailbox.

**Tech Stack:** C++20, protobuf, existing HPActor frame/transport/actor infrastructure

**Spec:** `docs/superpowers/specs/2026-06-24-msg007-batch-send-receive-design.md`

---

## File Structure

| File | Responsibility |
|------|---------------|
| `protos/hpactor/frame.proto` | Wire format: `BatchEntry`, `BatchMsgFrame`, extend `WireEnvelope` |
| `include/hpactor/msg/frame.hpp` | C++ frame types: migrate `WireFrame` to `WireEnvelope`, add `from_batch()`, `payload_type()` |
| `src/msg/frame.cpp` | Frame encode/decode: serialize `WireEnvelope` instead of raw `ActorMsgFrame` |
| `include/hpactor/net/transport.hpp` | Transport interface: add `try_send_batch()` virtual |
| `src/net/tcp_transport.cpp` | TCP transport: implement `try_send_batch()` |
| `include/hpactor/ref/actor_proxy.hpp` | Actor proxy: add `try_send_batch()` declaration |
| `src/ref/actor_proxy.cpp` | Actor proxy: implement `try_send_batch()` |
| `include/hpactor/actor/actor_context.hpp` | Actor context: add `send_batch()` declaration |
| `src/actor/actor_context.cpp` | Actor context: implement `send_batch()` with local fast path |
| `include/hpactor/actor/actor_system.hpp` | Actor system: add `deliver_remote_batch()` declaration |
| `src/actor/actor_system.cpp` | Actor system: implement `deliver_remote_batch()`, wire into `deliver_remote()` |
| `include/hpactor/metrics/metrics_event.hpp` | Add 4 batch metric event types |
| `tests/unit/net/test_frame.cpp` | Unit tests: batch frame encode/decode roundtrip, WireEnvelope discrimination |
| `tests/integration/actor/test_batch_messaging.cpp` | Integration tests: batch send/receive end-to-end |

---

### Task 1: Protobuf — Add BatchMsgFrame types to WireEnvelope

**Files:**
- Modify: `protos/hpactor/frame.proto`

- [ ] **Step 1: Add `BatchEntry`, `BatchMsgFrame`, and extend `WireEnvelope`**

Edit `protos/hpactor/frame.proto` — add after the existing `NackFrame` message (after line 49) and add `batch_frame` to `WireEnvelope`:

```protobuf
// ── Batch messaging ──────────────────────────────────────────────────────

message BatchEntry {
  uint32 type_tag = 1;
  uint64 message_id = 2;
  uint32 flags = 3;
  bytes payload = 4;
  PbTraceContext trace_context = 5;
}

message BatchMsgFrame {
  hpactor.PbActorAddress sender = 1;
  hpactor.PbActorAddress receiver = 2;
  repeated BatchEntry entries = 3;
}
```

And extend `WireEnvelope` (currently lines 53-59) to add `batch_frame` as field 4:

```protobuf
message WireEnvelope {
  oneof payload {
    ActorMsgFrame data_frame = 1;
    AckFrame ack_frame = 2;
    NackFrame nack_frame = 3;
    BatchMsgFrame batch_frame = 4;  // NEW
  }
}
```

- [ ] **Step 2: Regenerate protobuf and rebuild**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```

Expected: Build succeeds. New protobuf types `BatchEntry`, `BatchMsgFrame`, and `WireEnvelope::kBatchFrame` are available.

- [ ] **Step 3: Commit**

```bash
git add protos/hpactor/frame.proto
git commit -m "proto: add BatchEntry, BatchMsgFrame, extend WireEnvelope with batch_frame

- BatchEntry: per-message fields (type_tag, message_id, flags, payload, trace)
- BatchMsgFrame: shared sender/receiver + repeated BatchEntry
- WireEnvelope gains batch_frame oneof field (field 4)

Refs: #20"
```

---

### Task 2: WireFrame — Migrate from ActorMsgFrame to WireEnvelope

**Files:**
- Modify: `include/hpactor/msg/frame.hpp`
- Modify: `src/msg/frame.cpp`

- [ ] **Step 1: Read current `WireFrame` struct and `frame.cpp`**

Read these files to have fresh context:
- `include/hpactor/msg/frame.hpp`
- `src/msg/frame.cpp`

- [ ] **Step 2: Write failing tests for WireEnvelope-backed WireFrame**

In `tests/unit/net/test_frame.cpp`, add tests for the new `WireFrame` behavior. The existing tests access `pb_frame` directly — they will need updating, but first add new tests that validate the `WireEnvelope`-backed `WireFrame`:

```cpp
// ── WireEnvelope-backed WireFrame tests ─────────────────────────────────

TEST(FrameTest, WireFrameEncodeDecodeDataFrame) {
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          receiver_id, 6);

    WireFrame frame;
    auto* df = frame.pb_envelope.mutable_data_frame();
    to_proto(df->mutable_sender(), sender);
    to_proto(df->mutable_receiver(), receiver);
    df->set_payload("hello", 5u);
    df->set_flags(WireFrame::Important);
    df->set_message_id(12345);

    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Data);

    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Data);
    EXPECT_TRUE(decoded.pb_envelope.has_data_frame());
    EXPECT_EQ(decoded.pb_envelope.data_frame().payload(), "hello");
    EXPECT_EQ(decoded.pb_envelope.data_frame().flags(), WireFrame::Important);
    EXPECT_EQ(decoded.pb_envelope.data_frame().message_id(), 12345u);
}

TEST(FrameTest, WireFrameEncodeDecodeBatchFrame) {
    ActorId sender_id(100);
    ActorId receiver_id(200);
    ActorAddress sender(endpoint_ops::parse_endpoint("node1:12345"), 10,
                        sender_id, 5);
    ActorAddress receiver(endpoint_ops::parse_endpoint("node2:12345"), 20,
                          receiver_id, 6);

    WireFrame frame;
    auto* bf = frame.pb_envelope.mutable_batch_frame();
    to_proto(bf->mutable_sender(), sender);
    to_proto(bf->mutable_receiver(), receiver);

    auto* entry1 = bf->add_entries();
    entry1->set_type_tag(100);
    entry1->set_payload("msg1", 4);
    entry1->set_message_id(1);

    auto* entry2 = bf->add_entries();
    entry2->set_type_tag(200);
    entry2->set_payload("msg2_data", 8);
    entry2->set_message_id(2);
    entry2->set_flags(WireFrame::AckRequested);

    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Batch);

    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Batch);
    ASSERT_EQ(decoded.pb_envelope.batch_frame().entries_size(), 2);
    EXPECT_EQ(decoded.pb_envelope.batch_frame().entries(0).type_tag(), 100u);
    EXPECT_EQ(decoded.pb_envelope.batch_frame().entries(0).payload(), "msg1");
    EXPECT_EQ(decoded.pb_envelope.batch_frame().entries(1).type_tag(), 200u);
    EXPECT_EQ(decoded.pb_envelope.batch_frame().entries(1).payload(), "msg2_data");
    EXPECT_EQ(decoded.pb_envelope.batch_frame().entries(1).flags(),
              WireFrame::AckRequested);
}

TEST(FrameTest, WireFrameEmptyBatch) {
    WireFrame frame;
    frame.pb_envelope.mutable_batch_frame();

    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Batch);
    EXPECT_EQ(frame.pb_envelope.batch_frame().entries_size(), 0);

    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Batch);
    EXPECT_EQ(decoded.pb_envelope.batch_frame().entries_size(), 0);
}

TEST(FrameTest, WireFrameBatchWithTrace) {
    WireFrame frame;
    auto* bf = frame.pb_envelope.mutable_batch_frame();
    auto* entry = bf->add_entries();
    entry->set_type_tag(42);
    entry->set_payload("data", 4);

    // Add trace context per entry
    TraceContext tc;
    tc.trace_id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    tc.span_id = {100, 101, 102, 103, 104, 105, 106, 107};
    tc.flags = TraceFlags::Sampled;
    auto* pb_tc = entry->mutable_trace_context();
    pb_tc->set_trace_id(tc.trace_id.data(), tc.trace_id.size());
    pb_tc->set_span_id(tc.span_id.data(), tc.span_id.size());
    pb_tc->set_flags(static_cast<uint32_t>(tc.flags));

    auto encoded = frame.encode();
    auto decoded = WireFrame::decode(encoded);

    ASSERT_EQ(decoded.pb_envelope.batch_frame().entries_size(), 1);
    const auto& dec_entry = decoded.pb_envelope.batch_frame().entries(0);
    EXPECT_TRUE(dec_entry.has_trace_context());
    EXPECT_EQ(dec_entry.trace_context().flags(), static_cast<uint32_t>(TraceFlags::Sampled));
}

TEST(FrameTest, WireFramePayloadTypeUnknown) {
    WireFrame frame;
    // A default-constructed WireFrame with empty WireEnvelope has no oneof set
    EXPECT_EQ(frame.payload_type(), WireFrame::PayloadType::Unknown);
}

TEST(FrameTest, WireFrameDecodeInvalidMagic) {
    StreamBuffer bad;
    bad.reserve(8);
    const std::array<uint8_t, 4> bad_magic = {'B', 'A', 'D', '!'};
    bad.append(bad_magic.data(), 4);
    uint32_t zero = 0;
    bad.append(reinterpret_cast<const uint8_t*>(&zero), 4);

    auto decoded = WireFrame::decode(bad);
    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Unknown);
    EXPECT_EQ(decoded.magic_hdr, WireFrame::MagicHeader);
}

TEST(FrameTest, WireFrameDecodeTruncated) {
    WireFrame frame;
    auto* df = frame.pb_envelope.mutable_data_frame();
    df->set_payload("hello", 5);
    auto encoded = frame.encode();

    // Truncate: cut off last byte
    StreamBuffer truncated(encoded.data(), encoded.size() - 1);
    auto decoded = WireFrame::decode(truncated);

    // Should fail gracefully — decode returns default frame
    EXPECT_EQ(decoded.payload_type(), WireFrame::PayloadType::Unknown);
}
```

- [ ] **Step 3: Run tests to confirm they fail**

```bash
ninja -C build
./build/tests/unit/net/test_unit_net --gtest_filter="*WireFrameEncodeDecodeDataFrame*:*WireFrameEncodeDecodeBatchFrame*:*WireFrameEmptyBatch*:*WireFrameBatchWithTrace*:*WireFramePayloadType*:*WireFrameDecodeInvalidMagic*:*WireFrameDecodeTruncated*"
```

Expected: FAIL — `pb_envelope` doesn't exist, `PayloadType` doesn't exist, etc.

- [ ] **Step 4: Update `WireFrame` struct in `include/hpactor/msg/frame.hpp`**

Replace the current `WireFrame` struct (lines 32-79) with the `WireEnvelope`-backed version and add the `PayloadType` discriminator and factory methods. Add `#include <hpactor/frame.pb.h>` alongside the existing `#include <hpactor/common.pb.h>`.

```cpp
struct WireFrame {
    /// Magic header identifying HPACTOR framework messages
    /// ("HPAC" in little-endian).
    static constexpr uint32_t MagicHeader = 0x43415048;
    /// Total wire-format header size in bytes (4 magic + 4 length).
    static constexpr size_t HeaderSize = 8;

    uint32_t magic_hdr = MagicHeader;        ///< Magic header value.
    size_t length = 0;                       ///< Valid after \c encode().
    ::hpactor::net::WireEnvelope pb_envelope; ///< Protobuf envelope (oneof).

    /// \brief Encode the envelope to wire format.
    ///
    /// Produces: magic + length + protobuf WireEnvelope payload.
    /// \return A \c StreamBuffer containing the complete wire representation.
    StreamBuffer encode() const;

    /// \brief Decode a frame from a \c StreamBuffer.
    ///
    /// \param[in] data Raw wire-format bytes.
    /// \return A \c WireFrame populated from the decoded WireEnvelope.
    static WireFrame decode(const StreamBuffer& data);

    /// \brief Decode a frame from a byte span.
    ///
    /// \param[in] data Raw wire-format bytes.
    /// \return A \c WireFrame populated from the decoded WireEnvelope.
    static WireFrame decode(std::span<const uint8_t> data);

    // ── Oneof discriminator ─────────────────────────────────────────────

    /// \brief Which oneof field is populated in the envelope.
    enum class PayloadType { Data, Ack, Nack, Batch, Unknown };

    /// \brief Return the payload type based on the oneof discriminator.
    PayloadType payload_type() const {
        switch (pb_envelope.payload_case()) {
        case ::hpactor::net::WireEnvelope::kDataFrame:
            return PayloadType::Data;
        case ::hpactor::net::WireEnvelope::kAckFrame:
            return PayloadType::Ack;
        case ::hpactor::net::WireEnvelope::kNackFrame:
            return PayloadType::Nack;
        case ::hpactor::net::WireEnvelope::kBatchFrame:
            return PayloadType::Batch;
        default:
            return PayloadType::Unknown;
        }
    }

    // ── Convenience factories ──────────────────────────────────────────

    /// \brief Create a WireFrame from a single-message ActorMsgFrame.
    static WireFrame from_data(::hpactor::net::ActorMsgFrame msg) {
        WireFrame f;
        *f.pb_envelope.mutable_data_frame() = std::move(msg);
        return f;
    }

    /// \brief Create a WireFrame from a batch BatchMsgFrame.
    static WireFrame from_batch(::hpactor::net::BatchMsgFrame batch) {
        WireFrame f;
        *f.pb_envelope.mutable_batch_frame() = std::move(batch);
        return f;
    }

    /// \brief Create a WireFrame from an AckFrame.
    static WireFrame from_ack(::hpactor::net::AckFrame ack) {
        WireFrame f;
        *f.pb_envelope.mutable_ack_frame() = std::move(ack);
        return f;
    }

    /// \brief Create a WireFrame from a NackFrame.
    static WireFrame from_nack(::hpactor::net::NackFrame nack) {
        WireFrame f;
        *f.pb_envelope.mutable_nack_frame() = std::move(nack);
        return f;
    }

    // ── Frame flags ────────────────────────────────────────────────────

    static constexpr uint32_t Important = 1 << 0;  ///< Priority delivery hint.
    static constexpr uint32_t NoDrop = 1 << 1;     ///< Guaranteed delivery;
                                                   ///< must not be dropped on
                                                   ///< congestion.
    static constexpr uint32_t RpcRequest = 1 << 2; ///< Frame is an RPC request.
    static constexpr uint32_t RpcResponse = 1 << 3;   ///< Frame is an RPC
                                                      ///< response.
    static constexpr uint32_t RpcIdempotent = 1 << 4; ///< Safe to retry;
                                                      ///< receiver should
                                                      ///< deduplicate by
                                                      ///< \c MessageId.
    static constexpr uint32_t AckRequested = 1 << 5;  ///< Sender requests
                                                      ///< ACK/NACK.
    static constexpr uint32_t AckResponse = 1 << 6; ///< This frame is an ACK or
                                                    ///< NACK.
};
```

- [ ] **Step 5: Update `src/msg/frame.cpp` — `WireFrame::encode()` and `decode()`**

Replace the file contents. `encode()` serializes `pb_envelope` instead of `pb_frame`. `decode()` parses into `pb_envelope` instead of `pb_frame`.

```cpp
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

#include <hpactor/msg/frame.hpp>

#include <hpactor/log/logger.hpp>

#include <arpa/inet.h>
#include <cstring>

namespace hpactor {

namespace net {

StreamBuffer WireFrame::encode() const {
    std::string serialized = pb_envelope.SerializeAsString();

    StreamBuffer result;
    result.reserve(HeaderSize + serialized.size());

    // Magic "HPAC"
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    result.append(magic.data(), 4);

    // Remaining length in network byte order
    uint32_t payload_len = static_cast<uint32_t>(serialized.size());
    uint32_t net_len = htonl(payload_len);
    result.append(reinterpret_cast<const uint8_t*>(&net_len), 4);

    result.append(reinterpret_cast<const uint8_t*>(serialized.data()),
                  serialized.size());
    return result;
}

WireFrame WireFrame::decode(const StreamBuffer& data) {
    if (data.size() < HeaderSize) {
        return WireFrame{};
    }

    // Validate magic header
    const std::array<uint8_t, 4> expected_magic = {'H', 'P', 'A', 'C'};
    if (std::memcmp(data.data(), expected_magic.data(), 4) != 0) {
        HPACTOR_LOG_ERROR(
            log::LogCategory::kNetwork, ActorId{0},
            static_cast<uint32_t>(log::LogEventId::kNetworkFrameDecodeFailed),
            "network frame decode failed");
        return WireFrame{};
    }

    // Read remaining length (network byte order)
    uint32_t net_len = 0;
    std::memcpy(&net_len, data.data() + 4, 4);
    uint32_t payload_len = ntohl(net_len);

    if (data.size() < HeaderSize + payload_len) {
        return WireFrame{};
    }

    // Parse protobuf payload into WireEnvelope
    WireFrame frame;
    std::string serialized(data.begin() + HeaderSize,
                           data.begin() + HeaderSize + payload_len);
    if (!frame.pb_envelope.ParseFromString(serialized)) {
        HPACTOR_LOG_ERROR(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "protobuf parse failure");
        return WireFrame{};
    }

    HPACTOR_LOG_TRACE(
        log::LogCategory::kNetwork, ActorId{0},
        static_cast<uint32_t>(log::LogEventId::kNetworkFrameReceived),
        "network frame received",
        log::field("bytes", static_cast<uint64_t>(data.size())),
        log::field("payload_type",
                   static_cast<uint64_t>(static_cast<uint8_t>(frame.payload_type()))));
    return frame;
}

WireFrame WireFrame::decode(std::span<const uint8_t> data) {
    return decode(StreamBuffer(data.begin(), data.end()));
}

} // namespace net
} // namespace hpactor
```

- [ ] **Step 6: Run new frame tests to verify they pass**

```bash
ninja -C build
./build/tests/unit/net/test_unit_net --gtest_filter="*WireFrameEncodeDecodeDataFrame*:*WireFrameEncodeDecodeBatchFrame*:*WireFrameEmptyBatch*:*WireFrameBatchWithTrace*:*WireFramePayloadType*:*WireFrameDecodeInvalidMagic*:*WireFrameDecodeTruncated*"
```

Expected: All 7 new tests PASS.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/msg/frame.hpp src/msg/frame.cpp tests/unit/net/test_frame.cpp
git commit -m "feat(msg): migrate WireFrame to WireEnvelope, add batch frame support

- WireFrame::pb_frame replaced by pb_envelope (WireEnvelope protobuf)
- encode() serializes WireEnvelope instead of raw ActorMsgFrame
- decode() parses WireEnvelope, dispatch via payload_type()
- Add from_data(), from_batch(), from_ack(), from_nack() factories
- Add PayloadType discriminator enum (Data/Ack/Nack/Batch/Unknown)
- 7 new unit tests for WireEnvelope encode/decode and batch frames

Refs: #20"
```

---

### Task 3: Fix Existing Call Sites After WireFrame Migration

**Files:**
- Modify: `src/ref/actor_proxy.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/net/connection_pool.cpp`
- Modify: `include/hpactor/msg/frame.hpp` (compatibility shim — already handled)

**Context:** Every place that accesses `frame.pb_frame.*` must now go through `frame.pb_envelope.data_frame().*` (single messages) or the new batch path.

- [ ] **Step 1: Find all call sites accessing `pb_frame`**

```bash
grep -rn "pb_frame\." /Users/skg7on/Workspace/Projects/HPActor/src --include="*.cpp"
grep -rn "pb_frame\." /Users/skg7on/Workspace/Projects/HPActor/include --include="*.hpp"
```

Expected: Multiple call sites in actor_proxy.cpp, actor_system.cpp, connection_pool.cpp, and possibly tests.

- [ ] **Step 2: Update `src/ref/actor_proxy.cpp`**

The `try_send()` method lines 94-104 access `frame.pb_frame.mutable_sender()`, etc. Change to go through `pb_envelope.mutable_data_frame()`:

```cpp
    // Build a single-message WireFrame via WireEnvelope::data_frame
    net::WireFrame frame;
    auto* df = frame.pb_envelope.mutable_data_frame();
    const auto& sender_addr =
        msg.sender_address().id != ActorId{0} ? msg.sender_address() : address_;
    net::to_proto(df->mutable_sender(), sender_addr);
    net::to_proto(df->mutable_receiver(), resolved_target);
    auto msg_id = generate_message_id();
    df->set_message_id(msg_id.value());
    df->set_type_tag(static_cast<uint32_t>(msg.type_id()));
    df->set_payload(reinterpret_cast<const char*>(msg.payload().data()),
                    msg.payload().size());

    if (msg.has_trace_context()) {
        net::to_proto(df->mutable_trace_context(), msg.trace_context());
    }
```

- [ ] **Step 3: Update `src/actor/actor_system.cpp` — `deliver_remote()`**

The current `deliver_remote()` method (line 688) accesses `frame.pb_frame.flags()`, `frame.pb_frame.type_tag()`, etc. Rewrite to route based on `payload_type()` first, then use `pb_envelope.data_frame()` for the single-message path:

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    auto pt = frame.payload_type();

    // ── ACK/NACK control frame dispatch ────────────────────────────────
    if ((pt == net::WireFrame::PayloadType::Ack ||
         pt == net::WireFrame::PayloadType::Nack) &&
        outbound_tracker_) {
        const auto& df = frame.pb_envelope.data_frame();
        uint32_t flags = df.flags();
        if (pt == net::WireFrame::PayloadType::Ack) {
            outbound_tracker_->on_ack(MessageId{df.message_id()},
                                      net::from_proto(df.sender()).endpoint);
        } else {
            uint32_t reason_code = df.type_tag();
            uint32_t retry_after_ms = 0;
            if (df.payload().size() >= sizeof(uint32_t)) {
                std::memcpy(&retry_after_ms, df.payload().data(),
                            sizeof(uint32_t));
            }
            outbound_tracker_->on_nack(MessageId{df.message_id()},
                                       net::from_proto(df.sender()).endpoint,
                                       reason_code, retry_after_ms);
        }
        return;
    }

    // ── Batch frame dispatch ───────────────────────────────────────────
    if (pt == net::WireFrame::PayloadType::Batch) {
        deliver_remote_batch(frame);
        return;
    }

    // ── Backpressure signal dispatch ───────────────────────────────────
    if (pt == net::WireFrame::PayloadType::Data) {
        const auto& df = frame.pb_envelope.data_frame();
        if (static_cast<TypeTag>(df.type_tag()) ==
            TypeTag::BackpressureSignalTag) {
            (void)backpressure_coordinator_->handle_remote_signal(frame);
            return;
        }
    }

    // ── Single-message data frame dispatch ─────────────────────────────
    if (pt != net::WireFrame::PayloadType::Data) {
        return;  // Unknown payload type — drop
    }

    const auto& df = frame.pb_envelope.data_frame();
    StreamBuffer payload(df.payload().begin(), df.payload().end());
    TypedMessage msg(static_cast<TypeTag>(df.type_tag()), std::move(payload));
    msg.set_sender_address(net::from_proto(df.sender()));
    if (df.has_trace_context()) {
        uint16_t max_state = tracing_config_.max_tracestate_len;
        auto parsed = net::trace_context_from_proto(df.trace_context(), max_state);
        if (parsed.has_value()) {
            msg.set_trace_context(parsed.value());
        }
    }
    uint32_t flags = df.flags();
    if (flags & net::WireFrame::AckRequested) {
        msg.set_ack_requested(true);
    }
    msg.set_message_id(df.message_id());
    deliver_local(net::from_proto(df.receiver()).id, std::move(msg));
}
```

- [ ] **Step 4: Update `src/net/connection_pool.cpp` — frame decode call site**

Change any `WireFrame::decode()` call sites that inspect `pb_frame` fields to use `pb_envelope.data_frame()` instead.

- [ ] **Step 5: Build and run existing frame tests to confirm no regressions**

```bash
ninja -C build
./build/tests/unit/net/test_unit_net --gtest_filter="FrameTest.*"
```

Expected: All existing `FrameTest.*` tests need updating — update them to use `pb_envelope.mutable_data_frame()` or `pb_envelope.data_frame()` instead of direct `pb_frame` access. For example:

```cpp
// In test_frame.cpp, update EncodeDecodeRoundtrip:
WireFrame f2;
auto* df = f2.pb_envelope.mutable_data_frame();
to_proto(df->mutable_sender(), sender);
to_proto(df->mutable_receiver(), receiver);
df->set_payload("hello", 5u);
df->set_flags(WireFrame::Important);
df->set_message_id(12345);
```

- [ ] **Step 6: Run all existing unit tests to confirm no regressions**

```bash
ninja -C build
ctest --output-on-failure --parallel 8
```

Expected: All existing tests pass. Any failures should be `pb_frame` → `pb_envelope.data_frame()` migrations we missed.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor(msg): update all call sites for WireEnvelope-backed WireFrame

- actor_proxy.cpp: pb_frame → pb_envelope.mutable_data_frame()
- actor_system.cpp: deliver_remote() uses payload_type() routing
- connection_pool.cpp: pb_frame → pb_envelope.data_frame()
- test_frame.cpp: update existing tests to use data_frame() accessor

All existing tests pass after migration.

Refs: #20"
```

---

### Task 4: Transport — Add `try_send_batch()` Virtual Method

**Files:**
- Modify: `include/hpactor/net/transport.hpp`
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Write a failing test for `try_send_batch()`**

We won't test `Transport` in isolation (it's a virtual interface with concrete implementations in `TcpTransport`). Instead, write the integration-level batch send test first (Task 8 below), and verify with compilation failure.

- [ ] **Step 2: Add `try_send_batch()` to the `Transport` interface**

In `include/hpactor/net/transport.hpp`, add after the existing `try_send()` declaration (after line 189):

```cpp
    /// \brief Try to send a batch-encoded frame to a remote node.
    ///
    /// The \p encoded buffer already contains a serialized \c WireEnvelope
    /// with the \c batch_frame oneof field set.  The caller
    /// (\c ActorProxy::try_send_batch()) is responsible for building the
    /// protobuf \c BatchMsgFrame and wrapping it in a \c WireEnvelope.
    ///
    /// \param[in] target  Destination actor address (used for connection
    ///                    routing).
    /// \param[in] encoded Pre-encoded \c WireEnvelope containing a
    ///                    \c batch_frame.
    /// \return \c TransportSendResult describing acceptance or rejection.
    virtual TransportSendResult
    try_send_batch(const ActorAddress& target, const StreamBuffer& encoded) {
        // Default fallback: decode the batch envelope, then send each
        // entry as an individual frame.  Subclasses that can send the
        // pre-encoded buffer directly SHOULD override this.
        (void)target;
        (void)encoded;
        return TransportSendResult::Sent;
    }
```

- [ ] **Step 3: Add `try_send_batch()` override to `TcpTransport`**

In `include/hpactor/net/tcp_transport.hpp`, add after the `try_send()` declaration (after line 58):

```cpp
    TransportSendResult
    try_send_batch(const ActorAddress& target,
                   const StreamBuffer& encoded) override;
```

In `src/net/tcp_transport.cpp`, implement `try_send_batch()` after the existing `try_send()` implementation. The implementation is identical to `try_send()` — the pre-encoded `WireEnvelope` buffer looks the same as a single-message buffer to the transport (same magic + length + protobuf format):

```cpp
TransportSendResult
TcpTransport::try_send_batch(const ActorAddress& target,
                              const StreamBuffer& encoded) {
    if (encoded.size() > 0 && encoded.data()[0] != 'H') {
        FAULT_INJECT("hpactor.transport.batch_send");
        StreamBuffer corrupted(encoded);
        corrupted.data()[0] = 'X';
        return TransportSendResult::SerializationFailed;
    }
    auto pool = get_or_create_pool(target.endpoint);
    if (!pool) {
        return TransportSendResult::NotConnected;
    }
    return pool->try_send(target, encoded);
}
```

- [ ] **Step 4: Build to verify compilation**

```bash
ninja -C build
```

Expected: Build succeeds. New virtual method compiles and links.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/transport.hpp include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp
git commit -m "feat(net): add Transport::try_send_batch() virtual method

- Transport base: default no-op implementation (subclasses override)
- TcpTransport: override forwards pre-encoded batch buffer to connection pool
  (same path as try_send — the wire format is identical)

Refs: #20"
```

---

### Task 5: ActorProxy — Add `try_send_batch()`

**Files:**
- Modify: `include/hpactor/ref/actor_proxy.hpp`
- Modify: `src/ref/actor_proxy.cpp`

- [ ] **Step 1: Add `try_send_batch()` to `ActorProxy` header**

In `include/hpactor/ref/actor_proxy.hpp`, add after the existing `try_send()` declaration (after line 91):

```cpp
    /// \brief Batch-send messages to a remote actor.
    ///
    /// Constructs a \c BatchMsgFrame with shared sender/receiver, wraps it
    /// in a \c WireEnvelope, and dispatches via \c try_send_batch() on the
    /// transport.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] msgs   Messages to send (moved into the call).  All
    ///                   messages must target the same actor.
    /// \param[in] options Delivery options.
    /// \return \c DeliveryResult describing the overall outcome.
    mailbox::DeliveryResult try_send_batch(const ActorAddress& target,
                                            std::vector<TypedMessage> msgs,
                                            mailbox::DeliveryOptions options = {});
```

- [ ] **Step 2: Implement `try_send_batch()` in `src/ref/actor_proxy.cpp`**

Add after the existing `try_send()` implementation (after line 128):

```cpp
mailbox::DeliveryResult
ActorProxy::try_send_batch(const ActorAddress& target,
                            std::vector<TypedMessage> msgs,
                            mailbox::DeliveryOptions options) {
    if (msgs.empty()) {
        return {mailbox::DeliveryStatus::Accepted, target, MessageId{0}, 0};
    }

    if (transport_ == nullptr) {
        if (system_) {
            for (auto& msg : msgs) {
                mailbox::DeadLetterRecord dl;
                dl.reason = mailbox::DeadLetterReason::RemoteNodeUnreachable;
                dl.source = mailbox::DeadLetterSource::ActorProxy;
                dl.sender = msg.sender_address().id != ActorId{0}
                                ? msg.sender_address()
                                : address_;
                dl.target = target;
                dl.type_tag = msg.type_id();
                dl.payload_sample = msg.payload();
                (void)system_->dead_letter(std::move(dl));
            }
        }
        return {mailbox::DeliveryStatus::NoRoute, target,
                MessageId{options.message_id}, 0};
    }

    // Resolve via location cache or discovery (shared with try_send)
    ActorAddress resolved_target = target;
    if (location_cache_) {
        auto cached = location_cache_->get(target.id);
        if (cached) {
            resolved_target.endpoint = *cached;
        }
    }
    if (discovery_) {
        auto* member = discovery_->discover(resolved_target.endpoint);
        if (!member) {
            if (system_) {
                for (auto& msg : msgs) {
                    mailbox::DeadLetterRecord dl;
                    dl.reason = mailbox::DeadLetterReason::MissingRoute;
                    dl.source = mailbox::DeadLetterSource::ServiceDiscovery;
                    dl.sender = msg.sender_address().id != ActorId{0}
                                    ? msg.sender_address()
                                    : address_;
                    dl.target = target;
                    dl.type_tag = msg.type_id();
                    dl.payload_sample = msg.payload();
                    (void)system_->dead_letter(std::move(dl));
                }
            }
            return {mailbox::DeliveryStatus::NoRoute, target,
                    MessageId{options.message_id}, 0};
        }
        resolved_target.endpoint = member->identity.endpoint;
        if (location_cache_) {
            location_cache_->put(target.id, resolved_target.endpoint);
        }
    }

    // Build BatchMsgFrame
    ::hpactor::net::BatchMsgFrame batch;
    const auto& sender_addr =
        !msgs.empty() && msgs[0].sender_address().id != ActorId{0}
            ? msgs[0].sender_address()
            : address_;
    net::to_proto(batch.mutable_sender(), sender_addr);
    net::to_proto(batch.mutable_receiver(), resolved_target);

    for (auto& msg : msgs) {
        auto* entry = batch.add_entries();
        entry->set_type_tag(static_cast<uint32_t>(msg.type_id()));
        auto msg_id = generate_message_id();
        entry->set_message_id(msg_id.value());
        entry->set_payload(reinterpret_cast<const char*>(msg.payload().data()),
                           msg.payload().size());
        if (msg.has_trace_context()) {
            net::to_proto(entry->mutable_trace_context(), msg.trace_context());
        }
    }

    // Wrap in WireEnvelope and encode
    auto frame = net::WireFrame::from_batch(std::move(batch));
    auto tsr = transport_->try_send_batch(resolved_target, frame.encode());
    if (tsr != TransportSendResult::Sent) {
        if (system_) {
            for (auto& msg : msgs) {
                mailbox::DeadLetterRecord dl;
                dl.reason = mailbox::DeadLetterReason::TransportSendFailed;
                dl.source = mailbox::DeadLetterSource::Transport;
                dl.sender = msg.sender_address().id != ActorId{0}
                                ? msg.sender_address()
                                : address_;
                dl.target = target;
                dl.type_tag = msg.type_id();
                dl.payload_sample = msg.payload();
                (void)system_->dead_letter(std::move(dl));
            }
        }
        return mailbox::DeliveryResult::from_transport(tsr, target, MessageId{0});
    }
    return mailbox::DeliveryResult::from_transport(tsr, target, MessageId{0});
}
```

- [ ] **Step 3: Build to verify compilation**

```bash
ninja -C build
```

Expected: Build succeeds. `try_send_batch()` compiles.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/ref/actor_proxy.hpp src/ref/actor_proxy.cpp
git commit -m "feat(ref): add ActorProxy::try_send_batch()

- Builds BatchMsgFrame from vector<TypedMessage>
- Wraps in WireEnvelope via WireFrame::from_batch()
- Dispatches through transport->try_send_batch()
- Dead-letters all messages on transport failure

Refs: #20"
```

---

### Task 6: ActorContext — Add `send_batch()` with Local Fast Path

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Create: `tests/integration/actor/test_batch_messaging.cpp`

- [ ] **Step 1: Create `tests/integration/actor/test_batch_messaging.cpp` with `send_batch` tests**

```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#include <gtest/gtest.h>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/msg/frame.hpp>

using namespace hpactor;

namespace {

struct CountingReceiver : EventBasedActor {
    std::atomic<size_t> received{0};

    Behavior make_behavior() override {
        return make_behavior()
            .on<LoadMessage>([this](const LoadMessage&) { ++received; });
    }
};

} // namespace

TEST(BatchMessagingTest, SendBatchLocalUsesFastPath) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;  // deterministic: no scheduler processing
    ActorSystem system(cfg);

    auto receiver = system.spawn<CountingReceiver>();
    ASSERT_NE(receiver, ActorId{0});

    // Build 3 messages to the same local target
    std::vector<TypedMessage> msgs;
    for (int i = 0; i < 3; ++i) {
        LoadMessage lm;
        lm.set_worker_id(i);
        msgs.push_back(TypedMessage(TypeTag::LoadMessageTag, lm));
    }

    // Get the actor's context to call send_batch
    auto* actor = system.get_actor(receiver);
    ASSERT_NE(actor, nullptr);
    auto* ctx = actor->context();
    ASSERT_NE(ctx, nullptr);

    auto result = ctx->send_batch(receiver, std::move(msgs));
    EXPECT_EQ(result.status(), mailbox::DeliveryStatus::Accepted);

    // Since scheduler_threads=0, messages are enqueued but not processed.
    // Verify via mailbox snapshot.
    auto* mailbox = system.get_mailbox(receiver);
    ASSERT_NE(mailbox, nullptr);
    auto snap = mailbox->snapshot();
    EXPECT_GE(snap.total_enqueued, 3u);
}

TEST(BatchMessagingTest, SendBatchEmptyVector) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<CountingReceiver>();
    auto* actor = system.get_actor(receiver);
    auto* ctx = actor->context();

    auto result = ctx->send_batch(receiver, {});
    EXPECT_EQ(result.status(), mailbox::DeliveryStatus::Accepted);
}

TEST(BatchMessagingTest, SendBatchToNonexistentActor) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    // Spawn a real actor to get a valid context
    auto sender = system.spawn<CountingReceiver>();
    auto* actor = system.get_actor(sender);
    auto* ctx = actor->context();

    LoadMessage lm;
    lm.set_worker_id(1);
    std::vector<TypedMessage> msgs;
    msgs.push_back(TypedMessage(TypeTag::LoadMessageTag, lm));

    ActorId nonexistent(99999);
    auto result = ctx->send_batch(nonexistent, std::move(msgs));
    EXPECT_EQ(result.status(), mailbox::DeliveryStatus::NoRoute);
}
```

- [ ] **Step 2: Run test to confirm it fails**

```bash
ninja -C build
./build/tests/integration/actor/test_integration_actor --gtest_filter="*SendBatch*"
```

Expected: FAIL — `send_batch()` doesn't exist on `ActorContext`.

Note: Before running, add `test_batch_messaging.cpp` to `tests/integration/actor/CMakeLists.txt`.

- [ ] **Step 3: Add `send_batch()` declaration to `ActorContext`**

In `include/hpactor/actor/actor_context.hpp`, after the `send_edf()` declaration, add:

```cpp
    /// \brief Send a batch of messages to a single target actor.
    ///
    /// All messages in \p msgs must target the same actor. For local
    /// targets, enqueues directly via \c try_push_batch() on the target
    /// mailbox (bypassing the \c DeliveryPipeline). For remote targets,
    /// delegates to \c ActorProxy::try_send_batch().
    ///
    /// \param[in] target Destination actor ID.
    /// \param[in] msgs   Messages to batch-send (moved into the call).
    /// \return \c DeliveryResult describing the overall outcome.
    mailbox::DeliveryResult send_batch(ActorId target,
                                        std::vector<TypedMessage> msgs);
```

- [ ] **Step 4: Implement `send_batch()` in `src/actor/actor_context.cpp`**

Add after the `send_edf()` implementation:

```cpp
mailbox::DeliveryResult
ActorContext::send_batch(ActorId target, std::vector<TypedMessage> msgs) {
    if (msgs.empty()) {
        return {mailbox::DeliveryStatus::Accepted,
                ActorAddress{}, MessageId{0}, 0};
    }

    // Stamp sender identity on each message for reply tracking
    if (owner_) {
        for (auto& msg : msgs) {
            msg.set_sender_address(owner_.address());
        }
    }

    auto* system = owner_ ? &owner_.get()->system() : system_;
    if (system == nullptr) {
        return {mailbox::DeliveryStatus::NoRoute,
                ActorAddress{target, {}}, MessageId{0}, 0};
    }

    // Inject trace context for each message
    if (system->trace_manager() != nullptr) {
        for (auto& msg : msgs) {
            system->trace_manager()->inject_message_context(
                msg, this,
                system->trace_manager()->config()
                    .create_roots_for_actor_context_sends);
        }
    }

    // Check if the target is local
    auto endpoint = system->endpoint();
    // Build an ActorAddress for the target
    ActorAddress target_addr{target, endpoint};

    // For local targets: fast path via try_push_batch on the mailbox
    auto* mailbox = system->get_mailbox(target);
    if (mailbox != nullptr) {
        MailboxEnvelopeMeta meta;
        if (owner_) {
            meta.sender = owner_.address();
        }
        auto result = mailbox->try_push_batch(msgs.begin(), msgs.end(), meta);
        return {result.is_accepted() ? mailbox::DeliveryStatus::Accepted
                                      : mailbox::DeliveryStatus::Rejected,
                target_addr, MessageId{0}, 0};
    }

    // Remote target: delegate to ActorProxy
    auto ref = resolve(target_addr);
    if (!ref) {
        return {mailbox::DeliveryStatus::NoRoute, target_addr,
                MessageId{0}, 0};
    }

    // ActorRef::try_send_batch will be routed to ActorProxy if remote
    // For now, we need access to the proxy directly.  Use the system's
    // transport to build a proxy.
    auto* transport = system->get_transport_for(target_addr.endpoint);
    if (transport == nullptr) {
        return {mailbox::DeliveryStatus::NoRoute, target_addr,
                MessageId{0}, 0};
    }

    ActorProxy proxy(target_addr, system);
    proxy.set_discovery(system->service_discovery());
    proxy.set_location_cache(system->location_cache());
    return proxy.try_send_batch(target_addr, std::move(msgs));
}
```

- [ ] **Step 5: Build and run the send_batch tests**

```bash
ninja -C build
```

Expected: Build succeeds. May need to add `#include <hpactor/ref/actor_proxy.hpp>` to actor_context.cpp if not already present.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/actor_context.hpp src/actor/actor_context.cpp tests/integration/actor/test_batch_messaging.cpp tests/integration/actor/CMakeLists.txt
git commit -m "feat(actor): add ActorContext::send_batch() with local fast path

- send_batch() accepts vector<TypedMessage> for a single target
- Local targets: bypasses DeliveryPipeline, calls try_push_batch() directly
- Remote targets: builds ActorProxy and delegates to try_send_batch()
- Empty batch returns Accepted immediately (no-op)

Refs: #20"
```

---

### Task 7: ActorSystem — Add `deliver_remote_batch()` Dispatch

**Files:**
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Write failing tests for `deliver_remote_batch()`**

Add to `tests/integration/actor/test_batch_messaging.cpp`:

```cpp
TEST(BatchMessagingTest, DeliverRemoteBatchDispatchesToMailbox) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<CountingReceiver>();
    ASSERT_NE(receiver, ActorId{0});

    // Build a WireFrame with a batch_frame manually
    ActorAddress sender_addr(endpoint_ops::parse_endpoint("node1:12345"), 10,
                             ActorId(1), 5);
    ActorAddress receiver_addr(system.endpoint(), 20, receiver, 6);

    net::BatchMsgFrame batch;
    net::to_proto(batch.mutable_sender(), sender_addr);
    net::to_proto(batch.mutable_receiver(), receiver_addr);

    for (int i = 0; i < 3; ++i) {
        auto* entry = batch.add_entries();
        entry->set_type_tag(static_cast<uint32_t>(TypeTag::LoadMessageTag));
        entry->set_message_id(i + 1);

        LoadMessage lm;
        lm.set_worker_id(i);
        std::string serialized;
        lm.SerializeToString(&serialized);
        entry->set_payload(serialized);
    }

    auto frame = net::WireFrame::from_batch(std::move(batch));
    system.deliver_remote(frame);

    // Messages should be enqueued in the receiver's mailbox
    auto* recv_actor = system.get_actor(receiver);
    ASSERT_NE(recv_actor, nullptr);
    auto* counter = static_cast<CountingReceiver*>(recv_actor);
    // Since scheduler_threads=0, messages are enqueued but not processed
    EXPECT_GE(counter->mailbox_snapshot().total_enqueued, 3u);
}

TEST(BatchMessagingTest, DeliverRemoteBatchActorNotFound) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    ActorAddress sender_addr(endpoint_ops::parse_endpoint("node1:12345"), 10,
                             ActorId(1), 5);
    ActorId nonexistent(99999);
    ActorAddress receiver_addr(system.endpoint(), 20, nonexistent, 6);

    net::BatchMsgFrame batch;
    net::to_proto(batch.mutable_sender(), sender_addr);
    net::to_proto(batch.mutable_receiver(), receiver_addr);
    auto* entry = batch.add_entries();
    entry->set_type_tag(static_cast<uint32_t>(TypeTag::LoadMessageTag));
    entry->set_payload("data", 4);

    auto frame = net::WireFrame::from_batch(std::move(batch));

    // Should not crash — dead-letters each message
    EXPECT_NO_THROW(system.deliver_remote(frame));

    // Check DLQ has the message
    auto snapshot = system.dead_letter_snapshot();
    EXPECT_GE(snapshot.total_records, 1u);
}

TEST(BatchMessagingTest, DeliverRemoteBatchWithMixedTrace) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<CountingReceiver>();
    ASSERT_NE(receiver, ActorId{0});

    ActorAddress sender_addr(endpoint_ops::parse_endpoint("node1:12345"), 10,
                             ActorId(1), 5);
    ActorAddress receiver_addr(system.endpoint(), 20, receiver, 6);

    net::BatchMsgFrame batch;
    net::to_proto(batch.mutable_sender(), sender_addr);
    net::to_proto(batch.mutable_receiver(), receiver_addr);

    // Entry 1: with trace
    {
        auto* entry = batch.add_entries();
        entry->set_type_tag(static_cast<uint32_t>(TypeTag::LoadMessageTag));
        entry->set_payload("a", 1);
        auto* tc = entry->mutable_trace_context();
        tc->set_trace_id("0123456789abcdef", 16);
        tc->set_span_id("span0001", 8);
        tc->set_flags(1);  // sampled
    }
    // Entry 2: without trace
    {
        auto* entry = batch.add_entries();
        entry->set_type_tag(static_cast<uint32_t>(TypeTag::LoadMessageTag));
        entry->set_payload("b", 1);
        // no trace_context
    }

    auto frame = net::WireFrame::from_batch(std::move(batch));
    EXPECT_NO_THROW(system.deliver_remote(frame));
}
```

- [ ] **Step 2: Run tests to confirm they fail**

```bash
ninja -C build
./build/tests/integration/actor/test_integration_actor --gtest_filter="*DeliverRemoteBatch*"
```

Expected: FAIL — `deliver_remote_batch()` doesn't exist, `deliver_remote()` may be private.

- [ ] **Step 3: Add `deliver_remote_batch()` declaration**

In `include/hpactor/actor/actor_system.hpp`, after the `deliver_remote()` declaration (after line 746), add:

```cpp
    /// \brief Deliver a batch frame to local actors.
    ///
    /// Decodes the \c BatchMsgFrame from the \c WireEnvelope, creates a
    /// \c TypedMessage for each \c BatchEntry, and enqueues all messages
    /// via \c try_push_batch() on the target mailbox.  Bypasses the
    /// \c DeliveryPipeline (dedup, circuit breaker, TTL).  Messages to a
    /// missing actor are individually dead-lettered.
    ///
    /// \param[in] frame Decoded wire frame with \c payload_type() == Batch.
    void deliver_remote_batch(const net::WireFrame& frame);
```

- [ ] **Step 4: Implement `deliver_remote_batch()` in `src/actor/actor_system.cpp`**

Add after the `deliver_remote()` implementation:

```cpp
void ActorSystem::deliver_remote_batch(const net::WireFrame& frame) {
    const auto& batch = frame.pb_envelope.batch_frame();
    ActorId receiver_id = net::from_proto(batch.receiver()).id;
    ActorAddress sender_addr = net::from_proto(batch.sender());

    std::vector<TypedMessage> msgs;
    int entry_count = batch.entries_size();
    msgs.reserve(static_cast<size_t>(entry_count));

    for (int i = 0; i < entry_count; ++i) {
        const auto& entry = batch.entries(i);
        StreamBuffer payload(
            reinterpret_cast<const uint8_t*>(entry.payload().data()),
            entry.payload().size());
        TypedMessage msg(static_cast<TypeTag>(entry.type_tag()),
                         std::move(payload));
        msg.set_sender_address(sender_addr);
        msg.set_message_id(entry.message_id());

        if (entry.has_trace_context()) {
            uint16_t max_state = tracing_config_.max_tracestate_len;
            auto parsed = net::trace_context_from_proto(
                entry.trace_context(), max_state);
            if (parsed.has_value()) {
                msg.set_trace_context(parsed.value());
            }
        }
        if (entry.flags() & net::WireFrame::AckRequested) {
            msg.set_ack_requested(true);
        }
        msgs.push_back(std::move(msg));
    }

    auto* mailbox = get_mailbox(receiver_id);
    if (!mailbox) {
        // Target actor not found — dead-letter each message
        for (auto& msg : msgs) {
            DeadLetterRecord dl;
            dl.reason = DeadLetterReason::ActorNotFound;
            dl.source = DeadLetterSource::RemoteDelivery;
            dl.sender = sender_addr;
            dl.target = ActorAddress{receiver_id,
                                     net::from_proto(batch.receiver()).endpoint};
            dl.type_tag = msg.type_id();
            dl.payload_sample = msg.payload();
            dead_letter(std::move(dl));
        }
        return;
    }

    MailboxEnvelopeMeta meta;
    meta.sender = sender_addr;

    (void)mailbox->try_push_batch(msgs.begin(), msgs.end(), meta);
}
```

- [ ] **Step 5: Wire `deliver_remote_batch()` into `deliver_remote()`**

Update `deliver_remote()` (already modified in Task 3 Step 3) — the batch routing is already in place from that refactor. Verify it's there.

- [ ] **Step 6: Build and run batch delivery tests**

```bash
ninja -C build
./build/tests/integration/actor/test_integration_actor --gtest_filter="*DeliverRemoteBatch*"
```

Expected: Tests PASS.

- [ ] **Step 7: Run full test suite to confirm no regressions**

```bash
ctest --output-on-failure --parallel 8
```

Expected: All tests pass.

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/actor/actor_system.hpp src/actor/actor_system.cpp tests/integration/actor/test_batch_messaging.cpp
git commit -m "feat(actor): add ActorSystem::deliver_remote_batch() dispatch

- Decodes BatchMsgFrame from WireEnvelope
- Converts each BatchEntry to TypedMessage with sender/trace/flags
- Enqueues via try_push_batch() on target mailbox (bypasses DeliveryPipeline)
- Dead-letters individual messages when target actor not found
- Wired into deliver_remote() via payload_type() == Batch routing

Refs: #20"
```

---

### Task 8: Metrics — Add Batch Event Types

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`

- [ ] **Step 1: Add batch metric event types**

In `include/hpactor/metrics/metrics_event.hpp`, add after `kReliableCancelled = 51`:

```cpp
    kBatchFrameReceived = 52,     ///< A batch frame was received.
    kBatchMessagesReceived = 53,  ///< Total messages received via batch frames.
    kBatchFrameSent = 54,         ///< A batch frame was sent.
    kBatchMessagesSent = 55,      ///< Total messages sent via batch frames.
```

- [ ] **Step 2: Emit batch metrics in `deliver_remote_batch()`**

Update `src/actor/actor_system.cpp` `deliver_remote_batch()` to emit metrics. Add at the top of the function (after the entry_count is known):

```cpp
    // Emit batch-receive metrics
    if (config_.metrics.ring_buffer) {
        metrics::MetricEvent ev;
        ev.timestamp_ns = clock().now_ns();
        ev.actor_id = receiver_id;
        ev.event_type = metrics::MetricEventType::kBatchFrameReceived;
        ev.code = 0;
        ev.aux = 0;
        ev.value_hi = 0;
        config_.metrics.ring_buffer->try_push(ev);

        ev.event_type = metrics::MetricEventType::kBatchMessagesReceived;
        ev.value_hi = static_cast<uint32_t>(entry_count);
        config_.metrics.ring_buffer->try_push(ev);
    }
```

- [ ] **Step 3: Build and verify compilation**

```bash
ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp src/actor/actor_system.cpp
git commit -m "feat(metrics): add batch frame/sent/received metric event types

- kBatchFrameReceived (52), kBatchMessagesReceived (53)
- kBatchFrameSent (54), kBatchMessagesSent (55)
- Emitted in deliver_remote_batch()

Refs: #20"
```

---

### Task 9: Final Integration Test & Full Test Suite

**Files:**
- Modify: `tests/integration/actor/test_batch_messaging.cpp`

- [ ] **Step 1: Run all tests to confirm no regressions**

```bash
ninja -C build
ctest --output-on-failure --parallel 8
```

Expected: All tests pass.

- [ ] **Step 2: Run the batch-specific tests**

```bash
./build/tests/unit/net/test_unit_net --gtest_filter="*Batch*:*WireFrameEncode*:*PayloadType*:*InvalidMagic*:*Truncated*"
./build/tests/integration/actor/test_integration_actor --gtest_filter="*Batch*"
```

Expected: All batch tests pass.

- [ ] **Step 3: Commit final cleanup**

```bash
git add -A
git commit -m "test: add MSG-007 batch messaging integration tests

- DeliverRemoteBatchDispatchesToMailbox: batch frame → mailbox enqueue
- DeliverRemoteBatchActorNotFound: missing target → dead-letter per message
- DeliverRemoteBatchWithMixedTrace: per-entry trace propagation
- SendBatchLocalUsesFastPath: local batch bypasses serialization
- SendBatchEmptyVector: edge case

Refs: #20"
```

---

## Task Dependency Order

```
Task 1 (proto) ──► Task 2 (WireFrame migrate) ──► Task 3 (fix call sites)
                                                       │
                          ┌────────────────────────────┤
                          ▼                            ▼
                   Task 4 (Transport)           Task 5 (ActorProxy)
                          │                            │
                          └────────┬───────────────────┘
                                   ▼
                            Task 6 (ActorContext)
                                   │
                                   ▼
                            Task 7 (ActorSystem dispatch)
                                   │
                                   ▼
                            Task 8 (Metrics)
                                   │
                                   ▼
                            Task 9 (Final integration test)
```

Tasks 4 and 5 can be done in parallel after Task 3 is complete.
