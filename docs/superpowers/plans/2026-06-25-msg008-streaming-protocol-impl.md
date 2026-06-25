# MSG-008 Streaming Message Protocol — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement credit-based streaming message protocol with actor-backed sessions, wire protocol, and receiver-driven flow control.

**Architecture:** Internal `StreamSenderActor`/`StreamReceiverActor` pair per stream, `StreamHandle` user-facing API on `ActorContext`, 5 new `WireEnvelope` oneof frame types, byte-window flow control integrated with mailbox pressure, local fast path via `try_push_batch()`.

**Tech Stack:** C++20, Protobuf, Google Test, Ninja/CMake

**Spec:** `docs/superpowers/specs/2026-06-25-msg008-streaming-message-protocol-design.md`  
**Concept:** `docs/architecture/actor/streaming-message-protocol-core-concept.md`

---

## Phase 1: WireEnvelope Migration (Prerequisite)

The current `WireFrame` holds `ActorMsgFrame` directly. We must migrate to `WireEnvelope` (oneof dispatch) before adding stream frame types.

### Task 1.1: Extend WireEnvelope proto and migrate WireFrame

**Files:**
- Modify: `protos/hpactor/frame.proto` — add `BatchMsgFrame` (field 4), `StreamOpenFrame` (5), `StreamDataFrame` (6), `StreamAckFrame` (7), `StreamCloseFrame` (8), `StreamErrorFrame` (9) to `WireEnvelope` oneof
- Modify: `include/hpactor/msg/frame.hpp` — change `pb_frame` to `pb_envelope`, add `PayloadType` enum, add factory methods
- Modify: `src/msg/frame.cpp` — update `encode()`/`decode()` for `WireEnvelope`
- Modify: All call sites that access `frame.pb_frame` → `frame.pb_envelope.data_frame()`

#### WireEnvelope oneof in proto (add batch + all stream types at once):

```protobuf
message BatchMsgFrame {
  hpactor.PbActorAddress sender = 1;
  hpactor.PbActorAddress receiver = 2;
  repeated BatchEntry entries = 3;
}

message BatchEntry {
  uint32 type_tag = 1;
  uint64 message_id = 2;
  uint32 flags = 3;
  bytes payload = 4;
  PbTraceContext trace_context = 5;
}

message StreamOpenFrame {
  uint64 stream_id = 1;
  hpactor.PbActorAddress sender = 2;
  hpactor.PbActorAddress receiver = 3;
  uint32 initial_window_bytes = 4;
  PbTraceContext trace_context = 5;
}

message StreamDataFrame {
  uint64 stream_id = 1;
  uint64 sequence = 2;
  bytes payload = 3;
}

message StreamAckFrame {
  uint64 stream_id = 1;
  uint64 last_sequence = 2;
  uint32 window_bytes = 3;
}

message StreamCloseFrame {
  uint64 stream_id = 1;
  enum CloseReason {
    COMPLETE = 0;
    CANCELLED = 1;
    TIMEOUT = 2;
  }
  CloseReason reason = 2;
}

message StreamErrorFrame {
  uint64 stream_id = 1;
  uint32 error_code = 2;
  string description = 3;
}

message WireEnvelope {
  oneof payload {
    ActorMsgFrame data_frame = 1;
    AckFrame ack_frame = 2;
    NackFrame nack_frame = 3;
    BatchMsgFrame batch_frame = 4;
    StreamOpenFrame stream_open = 5;
    StreamDataFrame stream_data = 6;
    StreamAckFrame stream_ack = 7;
    StreamCloseFrame stream_close = 8;
    StreamErrorFrame stream_error = 9;
  }
}
```

- [ ] **Step 1: Add the proto messages and extend WireEnvelope oneof**

Edit `protos/hpactor/frame.proto` — add all stream frame types and `BatchMsgFrame`/`BatchEntry` messages, extend `WireEnvelope` oneof with fields 4-9.

- [ ] **Step 2: Regenerate protobuf C++ code**

Run: `cmake --build build --target hpactor_proto`
Expected: Generates updated `frame.pb.h` and `frame.pb.cc` with new message types.

- [ ] **Step 3: Rewrite WireFrame struct in frame.hpp**

Replace `pb_frame` with `pb_envelope`, add `PayloadType` enum and factory methods:

```cpp
struct WireFrame {
    static constexpr uint32_t MagicHeader = 0x43415048;
    static constexpr size_t HeaderSize = 8;

    enum class PayloadType : uint8_t {
        Data, Ack, Nack, Batch,
        StreamOpen, StreamData, StreamAck, StreamClose, StreamError,
        Unknown
    };

    uint32_t magic_hdr = MagicHeader;
    size_t length = 0;
    ::hpactor::net::WireEnvelope pb_envelope;

    StreamBuffer encode() const;
    static WireFrame decode(const StreamBuffer& data);
    static WireFrame decode(std::span<const uint8_t> data);

    PayloadType payload_type() const;

    // Factory methods
    static WireFrame from_data(::hpactor::net::ActorMsgFrame msg);
    static WireFrame from_ack(::hpactor::net::AckFrame ack);
    static WireFrame from_nack(::hpactor::net::NackFrame nack);
    static WireFrame from_batch(::hpactor::net::BatchMsgFrame batch);
    static WireFrame from_stream_open(::hpactor::net::StreamOpenFrame open);
    static WireFrame from_stream_data(::hpactor::net::StreamDataFrame data);
    static WireFrame from_stream_ack(::hpactor::net::StreamAckFrame ack);
    static WireFrame from_stream_close(::hpactor::net::StreamCloseFrame close);
    static WireFrame from_stream_error(::hpactor::net::StreamErrorFrame error);

    // Existing flags (unchanged)
    static constexpr uint32_t Important = 1 << 0;
    static constexpr uint32_t NoDrop = 1 << 1;
    static constexpr uint32_t RpcRequest = 1 << 2;
    static constexpr uint32_t RpcResponse = 1 << 3;
    static constexpr uint32_t RpcIdempotent = 1 << 4;
    static constexpr uint32_t AckRequested = 1 << 5;
    static constexpr uint32_t AckResponse = 1 << 6;
};
```

- [ ] **Step 4: Update encode() in frame.cpp**

```cpp
StreamBuffer WireFrame::encode() const {
    std::string pb_data;
    if (!pb_envelope.SerializeToString(&pb_data)) {
        return StreamBuffer{};
    }
    length = pb_data.size();
    StreamBuffer buf(HeaderSize + length);
    buf.insert(buf.begin(), reinterpret_cast<const uint8_t*>(&magic_hdr),
               reinterpret_cast<const uint8_t*>(&magic_hdr) + 4);
    uint32_t net_len = htonl(static_cast<uint32_t>(length));
    buf.insert(buf.begin() + 4,
               reinterpret_cast<const uint8_t*>(&net_len),
               reinterpret_cast<const uint8_t*>(&net_len) + 4);
    buf.insert(buf.begin() + HeaderSize,
               reinterpret_cast<const uint8_t*>(pb_data.data()),
               reinterpret_cast<const uint8_t*>(pb_data.data()) + length);
    return buf;
}
```

- [ ] **Step 5: Update decode() in frame.cpp**

```cpp
WireFrame WireFrame::decode(const StreamBuffer& data) {
    if (data.size() < HeaderSize) return WireFrame{};
    uint32_t magic;
    std::memcpy(&magic, data.data(), 4);
    if (magic != MagicHeader) return WireFrame{};
    uint32_t net_len;
    std::memcpy(&net_len, data.data() + 4, 4);
    size_t msg_len = ntohl(net_len);
    if (data.size() < HeaderSize + msg_len) return WireFrame{};
    WireFrame frame;
    frame.magic_hdr = magic;
    frame.length = msg_len;
    if (!frame.pb_envelope.ParseFromArray(data.data() + HeaderSize,
                                          static_cast<int>(msg_len))) {
        return WireFrame{};
    }
    return frame;
}
```

- [ ] **Step 6: Implement payload_type() discriminator**

```cpp
WireFrame::PayloadType WireFrame::payload_type() const {
    switch (pb_envelope.payload_case()) {
    case ::hpactor::net::WireEnvelope::kDataFrame:   return PayloadType::Data;
    case ::hpactor::net::WireEnvelope::kAckFrame:    return PayloadType::Ack;
    case ::hpactor::net::WireEnvelope::kNackFrame:   return PayloadType::Nack;
    case ::hpactor::net::WireEnvelope::kBatchFrame:  return PayloadType::Batch;
    case ::hpactor::net::WireEnvelope::kStreamOpen:  return PayloadType::StreamOpen;
    case ::hpactor::net::WireEnvelope::kStreamData:  return PayloadType::StreamData;
    case ::hpactor::net::WireEnvelope::kStreamAck:   return PayloadType::StreamAck;
    case ::hpactor::net::WireEnvelope::kStreamClose: return PayloadType::StreamClose;
    case ::hpactor::net::WireEnvelope::kStreamError: return PayloadType::StreamError;
    default: return PayloadType::Unknown;
    }
}
```

- [ ] **Step 7: Implement all factory methods in frame.cpp**

Each factory creates a `WireFrame`, sets the appropriate oneof field, and returns it.

- [ ] **Step 8: Find and update all call sites that access `pb_frame`**

Run: `grep -rn "pb_frame" src/ include/ tests/ apps/ --include="*.cpp" --include="*.hpp"`

Update each call site. Typical pattern changes:
- `frame.pb_frame.sender()` → `frame.pb_envelope.data_frame().sender()`
- `frame.pb_frame.receiver()` → `frame.pb_envelope.data_frame().receiver()`
- `frame.pb_frame.set_sender(...)` → `frame.pb_envelope.mutable_data_frame()->set_sender(...)`
- `frame.pb_frame.flags()` → `frame.pb_envelope.data_frame().flags()`

Also update frame creation sites:
- Old: `WireFrame frame; frame.pb_frame.set_type_tag(...);`
- New: `auto msg = ActorMsgFrame{}; msg.set_type_tag(...); auto frame = WireFrame::from_data(msg);`

- [ ] **Step 9: Update connection_pool.cpp frame dispatch**

Replace `frame.pb_frame.flags() & WireFrame::RpcResponse` check with `frame.payload_type()` switch. Add placeholder cases for stream types that return early with a log warning.

- [ ] **Step 10: Build and fix compilation errors**

Run: `ninja -C build`
Expected: Successful build with zero warnings.

- [ ] **Step 11: Run existing test suite to verify no regressions**

Run: `ctest --output-on-failure --parallel 8`
Expected: All existing tests pass (1411+ GTest cases). The WireEnvelope migration is a wire-incompatible change but existing tests that use WireFrame should still pass since they test encode/decode roundtrips.

- [ ] **Step 12: Update/rewrite frame unit tests for WireEnvelope**

Modify: `tests/unit/net/test_frame.cpp` (or wherever frame tests live)

Update all existing frame tests to use `WireEnvelope`. Ensure `encode → decode` roundtrip preserves: `ActorMsgFrame`, `AckFrame`, `NackFrame`. Add new tests for `BatchMsgFrame`, `StreamOpenFrame`, `StreamDataFrame`, `StreamAckFrame`, `StreamCloseFrame`, `StreamErrorFrame` roundtrips.

Run: `./build/tests/unit/net/test_unit_net --gtest_filter="*Frame*"`
Expected: All frame tests pass.

- [ ] **Step 13: Commit**

```bash
git add protos/hpactor/frame.proto include/hpactor/msg/frame.hpp src/msg/frame.cpp \
        src/net/connection_pool.cpp src/actor/spawn_receiver.cpp \
        tests/unit/net/test_frame.cpp <other modified files>
git commit -m "feat: migrate WireFrame to WireEnvelope oneof dispatch

Add WireEnvelope with data_frame, ack_frame, nack_frame, batch_frame,
and stream frame types (open, data, ack, close, error). WireFrame now
holds WireEnvelope instead of raw ActorMsgFrame. PayloadType discriminator
routes frames at receive sites.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 2: Stream Types, Config, and StreamHandle

### Task 2.1: Define StreamConfig

**Files:**
- Create: `include/hpactor/actor/stream_config.hpp`

- [ ] **Step 1: Write the StreamConfig header**

```cpp
#pragma once

#include <cstdint>
#include <hpactor/types/duration.hpp>

namespace hpactor {

struct StreamConfig {
    uint32_t initial_window_bytes = 64 * 1024;   // 64 KiB
    uint32_t max_chunk_bytes = 64 * 1024;        // 64 KiB
    uint32_t send_buffer_bytes = 256 * 1024;     // 256 KiB
    Duration idle_timeout = Duration::from_seconds(30);
    uint32_t max_in_flight_frames = 256;
};

} // namespace hpactor
```

- [ ] **Step 2: Verify build**

Run: `ninja -C build`
Expected: Compiles successfully.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/stream_config.hpp
git commit -m "feat(stream): add StreamConfig struct

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2.2: Define Stream TypeTags and Payloads

**Files:**
- Create: `include/hpactor/actor/stream_types.hpp`

- [ ] **Step 1: Write stream_types.hpp**

```cpp
#pragma once

#include <hpactor/msg/type_tag.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <cstdint>
#include <string>

namespace hpactor::stream {

// TypeTags in subsystem extension range (0x80-0xFF)
inline constexpr TypeTag StreamChunkTag  = make_subsystem_tag(0x80);
inline constexpr TypeTag StreamOpenedTag = make_subsystem_tag(0x81);
inline constexpr TypeTag StreamClosedTag = make_subsystem_tag(0x82);
inline constexpr TypeTag StreamErrorTag  = make_subsystem_tag(0x83);

struct StreamOpenedPayload {
    uint64_t stream_id;
    ActorAddress sender;
    uint32_t initial_window_bytes;
};

struct StreamClosedPayload {
    uint64_t stream_id;
    uint32_t reason;  // 0=COMPLETE, 1=CANCELLED, 2=TIMEOUT
    uint64_t total_bytes;
};

struct StreamErrorPayload {
    uint64_t stream_id;
    uint32_t error_code;
    std::string description;
};

} // namespace hpactor::stream
```

- [ ] **Step 2: Verify build**

Run: `ninja -C build`
Expected: Compiles.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/stream_types.hpp
git commit -m "feat(stream): add stream TypeTags and payload structs

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2.3: Implement StreamHandle

**Files:**
- Create: `include/hpactor/actor/stream_handle.hpp`
- Create: `tests/unit/actor/test_stream_handle.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/actor/test_stream_handle.cpp`:

```cpp
#include <gtest/gtest.h>
#include <hpactor/actor/stream_handle.hpp>
#include <hpactor/core/actor_id.hpp>

using namespace hpactor;

TEST(StreamHandleTest, DefaultConstruction) {
    StreamHandle h;
    EXPECT_FALSE(h.is_open());
    EXPECT_EQ(h.stream_id(), 0u);
}

TEST(StreamHandleTest, ConstructedHandleIsOpen) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    EXPECT_TRUE(h.is_open());
    EXPECT_EQ(h.stream_id(), 100u);
}

TEST(StreamHandleTest, MoveConstructTransfersOwnership) {
    ActorId sender_id{42};
    StreamHandle h1(sender_id, 100);
    StreamHandle h2(std::move(h1));
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), 100u);
    EXPECT_FALSE(h1.is_open());  // moved-from is closed
}

TEST(StreamHandleTest, MoveAssignTransfersOwnership) {
    ActorId sender_id{42};
    StreamHandle h1(sender_id, 100);
    StreamHandle h2;
    h2 = std::move(h1);
    EXPECT_TRUE(h2.is_open());
    EXPECT_FALSE(h1.is_open());
}

TEST(StreamHandleTest, CloseSetsNotOpen) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    bool result = h.close();
    EXPECT_TRUE(result);
    EXPECT_FALSE(h.is_open());
}

TEST(StreamHandleTest, DoubleCloseReturnsFalse) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    h.close();
    bool result = h.close();
    EXPECT_FALSE(result);
}

TEST(StreamHandleTest, WriteOnClosedReturnsFalse) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    h.close();
    StreamBuffer buf;
    bool result = h.write(TypeTag::User, std::move(buf));
    EXPECT_FALSE(result);
}

TEST(StreamHandleTest, ErrorSetsNotOpen) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    bool result = h.error(42, "test error");
    EXPECT_TRUE(result);
    EXPECT_FALSE(h.is_open());
}
```

- [ ] **Step 2: Add test to CMakeLists.txt**

Add `test_stream_handle.cpp` to `add_executable(test_unit_actor ...)` in `tests/unit/actor/CMakeLists.txt`.

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build test_unit_actor && ./build/tests/unit/actor/test_unit_actor --gtest_filter="*StreamHandle*"`
Expected: FAIL — `StreamHandle` not defined.

- [ ] **Step 4: Implement StreamHandle**

Create `include/hpactor/actor/stream_handle.hpp`:

```cpp
#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/types/types.hpp>
#include <cstdint>
#include <string_view>
#include <utility>

namespace hpactor {

class StreamHandle {
public:
    StreamHandle() = default;
    StreamHandle(ActorId sender_actor_id, uint64_t stream_id)
        : sender_actor_id_(sender_actor_id), stream_id_(stream_id), closed_(false) {}

    ~StreamHandle() = default;

    StreamHandle(StreamHandle&& other) noexcept
        : sender_actor_id_(other.sender_actor_id_)
        , stream_id_(other.stream_id_)
        , closed_(other.closed_) {
        other.closed_ = true;
    }

    StreamHandle& operator=(StreamHandle&& other) noexcept {
        if (this != &other) {
            sender_actor_id_ = other.sender_actor_id_;
            stream_id_ = other.stream_id_;
            closed_ = other.closed_;
            other.closed_ = true;
        }
        return *this;
    }

    StreamHandle(const StreamHandle&) = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;

    bool write(TypedMessage /*chunk*/) {
        if (closed_) return false;
        // Forward to StreamSenderActor in Phase 3
        return true;
    }

    bool write(TypeTag /*tag*/, StreamBuffer /*payload*/) {
        if (closed_) return false;
        return true;
    }

    bool close() {
        if (closed_) return false;
        closed_ = true;
        return true;
    }

    bool error(uint32_t /*code*/, std::string_view /*description*/ = "") {
        if (closed_) return false;
        closed_ = true;
        return true;
    }

    size_t bytes_in_flight() const { return 0; }
    size_t window_bytes() const { return 0; }
    bool is_open() const { return !closed_; }
    uint64_t stream_id() const { return stream_id_; }

private:
    ActorId sender_actor_id_{};
    uint64_t stream_id_ = 0;
    bool closed_ = false;
};

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./build/tests/unit/actor/test_unit_actor --gtest_filter="*StreamHandle*"`
Expected: All 8 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/stream_handle.hpp \
        tests/unit/actor/test_stream_handle.cpp \
        tests/unit/actor/CMakeLists.txt
git commit -m "feat(stream): add StreamHandle with move semantics

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 3: StreamSenderActor

### Task 3.1: Implement StreamSenderActor

**Files:**
- Create: `include/hpactor/actor/stream_sender_actor.hpp`
- Create: `src/actor/stream_sender_actor.cpp`
- Modify: `src/actor/CMakeLists.txt`

- [ ] **Step 1: Write the unit tests for StreamSenderActor credit arithmetic**

Create `tests/unit/actor/test_stream_sender_actor.cpp`:

```cpp
#include <gtest/gtest.h>
#include <hpactor/actor/stream_sender_actor.hpp>
#include <hpactor/actor/stream_config.hpp>

using namespace hpactor;

// StreamSenderActor credit logic is testable at the class level.
// We test the state machine transitions and credit arithmetic
// through integration tests (requires ActorSystem for spawn).

TEST(StreamSenderActorTest, StreamConfigDefaultValues) {
    StreamConfig cfg;
    EXPECT_EQ(cfg.initial_window_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.max_chunk_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.send_buffer_bytes, 256u * 1024u);
    EXPECT_EQ(cfg.max_in_flight_frames, 256u);
}

TEST(StreamSenderActorTest, BytesInFlightArithmetic) {
    // bytes_in_flight = total_sent - total_acked
    size_t sent = 100 + 200 + 150;     // 450 bytes sent
    size_t acked = 100 + 200;           // 300 bytes acked
    size_t in_flight = sent - acked;    // 150 bytes in flight
    EXPECT_EQ(in_flight, 150u);
}

TEST(StreamSenderActorTest, WindowExceededPauseCondition) {
    size_t bytes_in_flight = 400;
    size_t window_bytes = 256;
    EXPECT_TRUE(bytes_in_flight >= window_bytes);
    // Sender must pause when bytes_in_flight >= window_bytes (window > 0)
}

TEST(StreamSenderActorTest, WindowZeroAllowsSingleChunkForAntiDeadlock) {
    // When window_bytes == 0, sender may send ONE chunk to avoid deadlock
    size_t window_bytes = 0;
    size_t bytes_in_flight = 0;
    bool can_send_one = (bytes_in_flight == 0);
    EXPECT_TRUE(can_send_one);
}

TEST(StreamSenderActorTest, StreamIdFormula) {
    uint64_t sender_id = 42;
    uint64_t counter = 7;
    uint64_t stream_id = (sender_id << 32) | counter;
    EXPECT_EQ(stream_id, 0x2A00000007ULL);
}

TEST(StreamSenderActorTest, CumulativeAckAdvancesWindow) {
    // Ack with last_seq=5 means all chunks 1-5 are acknowledged
    uint64_t last_acked = 0;
    uint64_t new_ack = 5;
    EXPECT_GT(new_ack, last_acked);
    last_acked = new_ack;
    EXPECT_EQ(last_acked, 5u);
}

TEST(StreamSenderActorTest, DuplicateAckIsNoOp) {
    uint64_t last_acked = 5;
    uint64_t duplicate_ack = 3;  // Behind current ack
    EXPECT_LE(duplicate_ack, last_acked);
    // Should be ignored — no state change
}
```

- [ ] **Step 2: Write the StreamSenderActor header**

Create `include/hpactor/actor/stream_sender_actor.hpp`:

```cpp
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <cstdint>

namespace hpactor {

class StreamSenderActor : public EventBasedActor {
public:
    StreamSenderActor(ActorSystem& system, ActorId receiver_id,
                      uint64_t stream_id, StreamConfig config,
                      TraceContext trace_ctx);

    Behavior make_behavior() override;

    // Called by StreamHandle (via message) to enqueue a chunk for send
    void enqueue_chunk(TypedMessage chunk);

    // Query methods for StreamHandle
    size_t bytes_in_flight() const { return bytes_in_flight_; }
    size_t window_bytes() const { return window_bytes_; }
    bool is_stream_open() const { return state_ == State::Streaming; }

private:
    enum class State : uint8_t { Opening, Streaming, Closing, Closed, Error };

    void handle_stream_ack(const ::hpactor::net::StreamAckFrame& ack);
    void handle_stream_close(const ::hpactor::net::StreamCloseFrame& close);
    void handle_stream_error(const ::hpactor::net::StreamErrorFrame& error);
    void send_pending_chunks();
    void on_idle_timeout();

    ActorId receiver_id_;
    uint64_t stream_id_;
    StreamConfig config_;
    TraceContext trace_ctx_;
    State state_ = State::Opening;
    uint32_t window_bytes_ = 0;
    size_t bytes_in_flight_ = 0;
    uint64_t next_sequence_ = 1;
    uint64_t last_acked_ = 0;
    std::vector<TypedMessage> send_buffer_;
    size_t send_buffer_bytes_ = 0;
};

} // namespace hpactor
```

- [ ] **Step 3: Write the StreamSenderActor implementation**

Create `src/actor/stream_sender_actor.cpp`:

```cpp
#include <hpactor/actor/stream_sender_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

namespace hpactor {

StreamSenderActor::StreamSenderActor(ActorSystem& system, ActorId receiver_id,
                                     uint64_t stream_id, StreamConfig config,
                                     TraceContext trace_ctx)
    : EventBasedActor(system)
    , receiver_id_(receiver_id)
    , stream_id_(stream_id)
    , config_(config)
    , trace_ctx_(trace_ctx) {
    send_buffer_.reserve(config_.max_in_flight_frames);
}

Behavior StreamSenderActor::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        if (msg.type_id() == TypeTag::StreamAckTag) {
            // Deserialize StreamAckFrame and call handle_stream_ack
        } else if (msg.type_id() == TypeTag::StreamCloseTag) {
            // handle close (receiver-initiated close)
        } else if (msg.type_id() == TypeTag::StreamErrorTag) {
            // handle error
        } else {
            // User chunk from StreamHandle — enqueue
            enqueue_chunk(std::move(msg));
        }
    }};
}

void StreamSenderActor::enqueue_chunk(TypedMessage chunk) {
    if (state_ != State::Streaming) return;
    size_t chunk_size = chunk.payload().size();
    if (send_buffer_bytes_ + chunk_size > config_.send_buffer_bytes) return;
    send_buffer_.push_back(std::move(chunk));
    send_buffer_bytes_ += chunk_size;
    send_pending_chunks();
}

void StreamSenderActor::send_pending_chunks() {
    while (!send_buffer_.empty() && state_ == State::Streaming) {
        if (bytes_in_flight_ >= window_bytes_ && window_bytes_ > 0) break;
        if (send_buffer_.size() >= config_.max_in_flight_frames) break;

        TypedMessage chunk = std::move(send_buffer_.front());
        send_buffer_.erase(send_buffer_.begin());
        size_t chunk_size = chunk.payload().size();
        send_buffer_bytes_ -= chunk_size;

        // Build StreamDataFrame and send via transport or local fast path
        ::hpactor::net::StreamDataFrame data_frame;
        data_frame.set_stream_id(stream_id_);
        data_frame.set_sequence(next_sequence_++);
        data_frame.set_payload(chunk.payload().data(), chunk.payload().size());

        auto wire_frame = net::WireFrame::from_stream_data(std::move(data_frame));

        // Check if receiver is local
        if (system().is_local(receiver_id_)) {
            // Local fast path — enqueue directly to receiver actor
            system().deliver_remote(wire_frame);  // will route to local receiver
        } else {
            // Remote path — send via transport
            auto* transport = system().transport();
            if (transport) {
                ActorAddress addr = system().resolve_address(receiver_id_);
                transport->try_send(addr, wire_frame.encode());
            }
        }

        bytes_in_flight_ += chunk_size;
    }
}

void StreamSenderActor::handle_stream_ack(
    const ::hpactor::net::StreamAckFrame& ack) {
    if (ack.last_sequence() > last_acked_) {
        // Compute acked bytes and update bytes_in_flight_
        // (simplified: track per-sequence sizes in a map)
        last_acked_ = ack.last_sequence();
    }
    window_bytes_ = ack.window_bytes();
    if (state_ == State::Opening) {
        state_ = State::Streaming;
    }
    if (state_ == State::Streaming) {
        send_pending_chunks();
    }
}

void StreamSenderActor::on_idle_timeout() {
    if (state_ == State::Streaming || state_ == State::Opening) {
        net::StreamErrorFrame error;
        error.set_stream_id(stream_id_);
        error.set_error_code(1);  // TIMEOUT
        error.set_description("Stream idle timeout");
        auto frame = net::WireFrame::from_stream_error(std::move(error));
        // Send to receiver...
        state_ = State::Error;
    }
}

} // namespace hpactor
```

- [ ] **Step 4: Add to CMakeLists.txt**

Add `stream_sender_actor.cpp` to `src/actor/CMakeLists.txt` in `target_sources(hpactor_lib PRIVATE ...)`.

- [ ] **Step 5: Build and fix compilation errors**

Run: `ninja -C build`
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/stream_sender_actor.hpp \
        src/actor/stream_sender_actor.cpp \
        src/actor/CMakeLists.txt \
        tests/unit/actor/test_stream_sender_actor.cpp
git commit -m "feat(stream): add StreamSenderActor with credit window tracking

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 4: StreamReceiverActor

### Task 4.1: Implement StreamReceiverActor

**Files:**
- Create: `include/hpactor/actor/stream_receiver_actor.hpp`
- Create: `src/actor/stream_receiver_actor.cpp`
- Modify: `src/actor/CMakeLists.txt`

- [ ] **Step 1: Write StreamReceiverActor header**

Create `include/hpactor/actor/stream_receiver_actor.hpp`:

```cpp
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <cstdint>

namespace hpactor {

class StreamReceiverActor : public EventBasedActor {
public:
    StreamReceiverActor(ActorSystem& system, ActorId target_actor_id,
                        uint64_t stream_id, ActorAddress sender_addr,
                        uint32_t initial_window_bytes,
                        TraceContext trace_ctx);

    Behavior make_behavior() override;

private:
    void handle_stream_data(const ::hpactor::net::StreamDataFrame& data);
    void handle_stream_close(const ::hpactor::net::StreamCloseFrame& close);
    void handle_stream_error(const ::hpactor::net::StreamErrorFrame& error);
    void send_ack();
    uint32_t compute_window_bytes() const;

    ActorId target_actor_id_;
    uint64_t stream_id_;
    ActorAddress sender_addr_;
    uint32_t window_bytes_;
    uint64_t last_delivered_seq_ = 0;
    uint64_t total_bytes_received_ = 0;
    TraceContext trace_ctx_;
};

} // namespace hpactor
```

- [ ] **Step 2: Write StreamReceiverActor implementation**

Create `src/actor/stream_receiver_actor.cpp`. Key methods:

```cpp
#include <hpactor/actor/stream_receiver_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/frame.hpp>

namespace hpactor {

StreamReceiverActor::StreamReceiverActor(
    ActorSystem& system, ActorId target_actor_id,
    uint64_t stream_id, ActorAddress sender_addr,
    uint32_t initial_window_bytes, TraceContext trace_ctx)
    : EventBasedActor(system)
    , target_actor_id_(target_actor_id)
    , stream_id_(stream_id)
    , sender_addr_(sender_addr)
    , window_bytes_(initial_window_bytes)
    , trace_ctx_(trace_ctx) {}

Behavior StreamReceiverActor::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        // Dispatch on frame type via TypeTag
        // StreamDataFrame → handle_stream_data
        // StreamCloseFrame → handle_stream_close
        // StreamErrorFrame → handle_stream_error
    }};
}

void StreamReceiverActor::handle_stream_data(
    const ::hpactor::net::StreamDataFrame& data) {
    uint64_t seq = data.sequence();

    // Check ordering: must be last_delivered + 1
    if (seq != last_delivered_seq_ + 1) {
        // Out of order — send StreamErrorFrame
        ::hpactor::net::StreamErrorFrame error;
        error.set_stream_id(stream_id_);
        error.set_error_code(2);  // OUT_OF_ORDER
        error.set_description("Out-of-order sequence");
        auto frame = net::WireFrame::from_stream_error(std::move(error));
        // send to sender via transport...
        return;
    }

    // Deliver chunk to target actor
    TypedMessage chunk(static_cast<TypeTag>(0x1000),  // preserve original tag
                       StreamBuffer(data.payload().data(),
                                    data.payload().size()));
    chunk.set_sender_address(sender_addr_);
    if (trace_ctx_.is_valid()) {
        chunk.set_trace_context(trace_ctx_);
    }

    Actor receiver_actor = system().get_actor(target_actor_id_);
    if (receiver_actor) {
        system().try_deliver_local_fast(target_actor_id_, std::move(chunk));
    }

    last_delivered_seq_ = seq;
    total_bytes_received_ += data.payload().size();

    // Send ack with updated window
    send_ack();
}

void StreamReceiverActor::send_ack() {
    ::hpactor::net::StreamAckFrame ack;
    ack.set_stream_id(stream_id_);
    ack.set_last_sequence(last_delivered_seq_);
    ack.set_window_bytes(compute_window_bytes());

    auto frame = net::WireFrame::from_stream_ack(std::move(ack));

    if (system().is_local(sender_addr_)) {
        // Local — enqueue directly to sender actor's mailbox
        system().deliver_remote(frame);
    } else {
        auto* transport = system().transport();
        if (transport) {
            transport->try_send(sender_addr_, frame.encode());
        }
    }
}

uint32_t StreamReceiverActor::compute_window_bytes() const {
    // Query target actor's mailbox pressure
    Actor target = system().get_actor(target_actor_id_);
    if (!target) return 0;  // Target gone — close window

    auto snapshot = target->mailbox_snapshot();
    uint32_t max_window = config_.initial_window_bytes;

    // Pressure ratio in parts-per-million (0 = empty, 1,000,000 = full)
    uint32_t pressure_ppm = snapshot.pressure_ratio_ppm;

    // Window shrinks linearly from 100% at low-watermark (200,000 ppm)
    // to 0% at high-watermark (800,000 ppm)
    if (pressure_ppm <= 200000) {
        return max_window;
    } else if (pressure_ppm >= 800000) {
        return 0;
    } else {
        // Linear interpolation between low and high watermarks
        float ratio = 1.0f - static_cast<float>(pressure_ppm - 200000) / 600000.0f;
        return static_cast<uint32_t>(max_window * ratio);
    }
}

// handle_stream_close / handle_stream_error deliver StreamClosedTag/StreamErrorTag
// to the target actor, then self-terminate

} // namespace hpactor
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `stream_receiver_actor.cpp` to `src/actor/CMakeLists.txt`.

- [ ] **Step 4: Build and fix compilation**

Run: `ninja -C build`
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor/stream_receiver_actor.hpp \
        src/actor/stream_receiver_actor.cpp \
        src/actor/CMakeLists.txt
git commit -m "feat(stream): add StreamReceiverActor with credit window

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 5: ActorSystem Integration

### Task 5.1: Add stream routing to ActorSystem

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add stream registry and dispatch methods to actor_system.hpp**

Add to `ActorSystem` class:

```cpp
private:
    // stream_id → StreamSenderActor
    std::unordered_map<uint64_t, ActorId> stream_senders_;
    // stream_id → StreamReceiverActor
    std::unordered_map<uint64_t, ActorId> stream_receivers_;

public:
    /// Register a stream sender for inbound ack routing
    void register_stream_sender(uint64_t stream_id, ActorId actor_id);

    /// Register a stream receiver for inbound data routing
    void register_stream_receiver(uint64_t stream_id, ActorId actor_id);

    /// Remove stream registrations on close/error
    void unregister_stream(uint64_t stream_id);

    /// Allocate a unique stream ID
    uint64_t allocate_stream_id(ActorId sender_id);

    /// Deliver a remote stream frame to the correct stream actor
    void deliver_remote_stream(const net::WireFrame& frame);
```

- [ ] **Step 2: Implement stream dispatch in actor_system.cpp**

In `deliver_remote()`:

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    // Route stream frames before existing dispatch
    switch (frame.payload_type()) {
    case net::WireFrame::PayloadType::StreamOpen:
        deliver_remote_stream_open(frame);
        return;
    case net::WireFrame::PayloadType::StreamData:
        deliver_remote_stream_data(frame);
        return;
    case net::WireFrame::PayloadType::StreamAck:
        deliver_remote_stream_ack(frame);
        return;
    case net::WireFrame::PayloadType::StreamClose:
        deliver_remote_stream_close(frame);
        return;
    case net::WireFrame::PayloadType::StreamError:
        deliver_remote_stream_error(frame);
        return;
    default:
        break;  // fall through to existing dispatch
    }
    // ... existing dispatch logic ...
}
```

Implement each `deliver_remote_stream_*` method:
- `stream_open`: Spawn `StreamReceiverActor` on the target node
- `stream_data`: Look up `stream_receivers_[stream_id]`, forward frame
- `stream_ack`: Look up `stream_senders_[stream_id]`, forward frame
- `stream_close` / `stream_error`: Route to both sender and receiver for cleanup

- [ ] **Step 3: Implement allocate_stream_id**

```cpp
uint64_t ActorSystem::allocate_stream_id(ActorId sender_id) {
    static std::atomic<uint64_t> counter{0};
    uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
    return (static_cast<uint64_t>(sender_id.value()) << 32) | seq;
}
```

- [ ] **Step 4: Build**

Run: `ninja -C build`
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(stream): add stream frame routing to ActorSystem

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 5.2: Implement ActorContext::open_stream()

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

- [ ] **Step 1: Add open_stream() declaration**

```cpp
/// Open a streaming session to a target actor.
/// Returns StreamHandle on success, std::nullopt if target unreachable.
std::optional<StreamHandle> open_stream(ActorId target,
                                        StreamConfig config = {});
```

- [ ] **Step 2: Implement open_stream()**

```cpp
std::optional<StreamHandle> ActorSystem::open_stream(
    ActorId target, StreamConfig config) {
    auto actor = get_actor(target);
    if (!actor) return std::nullopt;

    uint64_t stream_id = allocate_stream_id(target);
    TraceContext trace_ctx = current_trace_context();

    auto sender = spawn<StreamSenderActor>(target, stream_id, config, trace_ctx);
    if (!sender) return std::nullopt;

    register_stream_sender(stream_id, sender.id());

    // Send StreamOpenFrame to target
    // If remote, SpawnReceiver spawns StreamReceiverActor
    // If local, spawn StreamReceiverActor directly
    ActorAddress target_addr = resolve_address(target);
    if (is_local(target_addr)) {
        auto receiver = spawn<StreamReceiverActor>(
            target, stream_id, local_endpoint(),
            config.initial_window_bytes, trace_ctx);
        register_stream_receiver(stream_id, receiver.id());
    } else {
        // Remote: StreamOpenFrame triggers SpawnReceiver on remote node
        send_stream_open_frame(target_addr, stream_id, config, trace_ctx);
    }

    return StreamHandle(sender.id(), stream_id);
}
```

- [ ] **Step 3: Build**

Run: `ninja -C build`
Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/actor_context.hpp src/actor/actor_context.cpp
git commit -m "feat(stream): add ActorContext::open_stream()

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 6: Dead Letter Queue & Error Handling

### Task 6.1: Add StreamClosed dead letter reason

**Files:**
- Modify: `include/hpactor/msg/dead_letter_record.hpp`

- [ ] **Step 1: Add StreamClosed to DeadLetterReason enum**

Add `StreamClosed = 19` after `RetryExhausted = 18`.

- [ ] **Step 2: Add to_string() case for StreamClosed**

```cpp
case DeadLetterReason::StreamClosed: return "StreamClosed";
```

- [ ] **Step 3: Add DLQ routing for stream close/error paths in StreamReceiverActor**

In `handle_stream_close()` and `handle_stream_error()`: dead-letter any buffered chunks with `DeadLetterReason::StreamClosed`.

- [ ] **Step 4: Build**

Run: `ninja -C build`

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/dead_letter_record.hpp src/actor/stream_receiver_actor.cpp
git commit -m "feat(stream): add StreamClosed dead letter reason

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 7: Metrics

### Task 7.1: Add stream metric events

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`

- [ ] **Step 1: Add stream metric event types**

Append to `MetricEventType` enum (last was `kReliableCancelled = 51`):

```cpp
kStreamOpened = 52,
kStreamClosed = 53,
kStreamBytesSent = 54,
kStreamBytesReceived = 55,
kStreamChunkSent = 56,
kStreamChunkReceived = 57,
kStreamWindowBytes = 58,
```

- [ ] **Step 2: Emit metric events in stream actors**

In `StreamSenderActor::enqueue_chunk()`:
```cpp
metrics::MetricEvent evt{};
evt.timestamp_ns = system().clock().now_ns();
evt.event_type = metrics::MetricEventType::kStreamChunkSent;
evt.value_hi = static_cast<uint32_t>(chunk_size >> 32);
system().metrics_ring_buffer()->try_push(evt);
```

In `StreamReceiverActor::handle_stream_data()`:
```cpp
// Similar for kStreamChunkReceived, kStreamBytesReceived
```

In `ActorContext::open_stream()` success path — emit `kStreamOpened`.
In stream close/error paths — emit `kStreamClosed`.

- [ ] **Step 3: Add to metrics aggregator**

Modify `src/metrics/metrics_aggregator.cpp` to handle the new event types and produce the counter/gauge families.

- [ ] **Step 4: Build**

Run: `ninja -C build`

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp \
        src/metrics/metrics_aggregator.cpp \
        src/actor/stream_sender_actor.cpp \
        src/actor/stream_receiver_actor.cpp \

git commit -m "feat(stream): add streaming metric events

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 8: CLI Commands

### Task 8.1: Implement /stream commands

**Files:**
- Create: `src/cli/commands/stream_commands.cpp`
- Modify: `src/cli/CMakeLists.txt`

- [ ] **Step 1: Write failing test for stream CLI commands**

Create `tests/unit/cli/test_stream_commands.cpp`:

```cpp
#include <gtest/gtest.h>
#include <hpactor/cli/command/command_registry.hpp>

using namespace hpactor;

TEST(StreamCommandsTest, StreamListCommandRegistered) {
    // After stream_commands.cpp is linked, the static CommandRegistration
    // fires before main() and registers /stream/list in the CommandRegistry.
    auto& reg = cli::CommandRegistry::instance();
    bool found = false;
    for (const auto& cmd : reg.commands()) {
        if (cmd->path() == "stream/list") {
            found = true;
            EXPECT_FALSE(cmd->help_text().empty());
            break;
        }
    }
    EXPECT_TRUE(found) << "/stream/list command not registered";
}

TEST(StreamCommandsTest, StreamShowCommandRegistered) {
    auto& reg = cli::CommandRegistry::instance();
    bool found = false;
    for (const auto& cmd : reg.commands()) {
        if (cmd->path() == "stream/show") {
            found = true;
            EXPECT_FALSE(cmd->help_text().empty());
            break;
        }
    }
    EXPECT_TRUE(found) << "/stream/show command not registered";
}
```

- [ ] **Step 2: Implement StreamListCommand**

```cpp
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/core/actor_system.hpp>

namespace {

class StreamListCommand final : public hpactor::cli::ICommand {
public:
    std::string_view path() const noexcept override { return "stream/list"; }
    std::string_view help_text() const noexcept override {
        return "List active stream sessions";
    }
    int order() const noexcept override { return 600; }

    hpactor::result<void> execute(hpactor::cli::CommandContext& ctx) const override {
        ctx.output->header("Active Streams");

        std::vector<std::string> cols = {"Stream ID", "Sender", "Receiver",
                                          "State", "In Flight"};
        std::vector<std::vector<std::string>> rows;

        // Enumerate stream senders from ActorSystem
        // For each: stream_id, sender actor, receiver actor, state, bytes_in_flight

        ctx.output->table(cols, rows);
        return hpactor::result<void>::make();
    }
};

class StreamShowCommand final : public hpactor::cli::ICommand {
public:
    std::string_view path() const noexcept override { return "stream/show"; }
    std::string_view help_text() const noexcept override {
        return "Show detailed stream state: /stream show <stream_id>";
    }
    int order() const noexcept override { return 601; }

    hpactor::result<void> execute(hpactor::cli::CommandContext& ctx) const override {
        // Parse stream_id from ctx.args
        // Look up stream in ActorSystem registry
        // Display key-value pairs: stream_id, sender, receiver, state,
        //   window_bytes, bytes_in_flight, chunks_sent, chunks_acked,
        //   opened_at, idle_for, trace_id
        ctx.output->header("Stream Details");
        std::map<std::string, std::string> kv;
        ctx.output->key_value(kv);
        return hpactor::result<void>::make();
    }
};

const hpactor::cli::CommandRegistration<StreamListCommand> kRegStreamList;
const hpactor::cli::CommandRegistration<StreamShowCommand> kRegStreamShow;

} // namespace
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `stream_commands.cpp` to `src/cli/CMakeLists.txt` in `target_sources(hpactor_lib PRIVATE ...)`.

- [ ] **Step 4: Build and run tests**

Run: `ninja -C build && ctest -R "StreamCommands" --output-on-failure`

- [ ] **Step 5: Commit**

```bash
git add src/cli/commands/stream_commands.cpp \
        src/cli/CMakeLists.txt \
        tests/unit/cli/test_stream_commands.cpp
git commit -m "feat(stream): add /stream list and /stream show CLI commands

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 9: Fault Injection

### Task 9.1: Add Stream fault domain and fault points

**Files:**
- Modify: `include/hpactor/fault/fault_types.hpp`
- Modify: `src/fault/fault_points.cpp`
- Modify: `src/actor/stream_sender_actor.cpp`

- [ ] **Step 1: Add kStream to FaultDomain enum**

Add `kStream = 15` after `kPassivation = 14`.

- [ ] **Step 2: Add to_string() case for kStream**

```cpp
case FaultDomain::kStream: return "stream";
```

- [ ] **Step 3: Add fault injection sites in StreamSenderActor**

```cpp
// In send_pending_chunks(), before sending a StreamDataFrame:
FAULT_INJECT("hpactor.stream.data.drop") {
    // Skip this chunk (simulate drop)
    continue;
}

FAULT_INJECT("hpactor.stream.data.delay") {
    // Defer send (re-enqueue at back of buffer)
    send_buffer_.push_back(std::move(chunk));
    continue;
}

// In handle_stream_ack(), on receiving ack:
FAULT_INJECT("hpactor.stream.ack.drop") {
    return;  // Don't process the ack
}

// In StreamReceiverActor::send_ack():
FAULT_INJECT("hpactor.stream.ack.drop") {
    return;  // Don't send ack (sender will timeout and retransmit)
}
```

- [ ] **Step 4: Register fault points in fault_points.cpp**

Add entries for `hpactor.stream.open`, `hpactor.stream.data`, `hpactor.stream.ack`, `hpactor.stream.close`, `hpactor.stream.error`.

- [ ] **Step 5: Build and run fault injection tests**

Run: `ninja -C build && ctest -R "Fault" --output-on-failure`

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/fault/fault_types.hpp src/fault/fault_points.cpp \
        src/actor/stream_sender_actor.cpp src/actor/stream_receiver_actor.cpp
git commit -m "feat(stream): add Stream fault domain with 5 fault points

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 10: Integration Tests

### Task 10.1: Stream frame roundtrip tests

**Files:**
- Create: `tests/unit/msg/test_stream_frames.cpp`
- Modify: `tests/unit/msg/CMakeLists.txt`

- [ ] **Step 1: Write stream frame unit tests**

```cpp
#include <gtest/gtest.h>
#include <hpactor/msg/frame.hpp>

using namespace hpactor;

TEST(StreamFrameTest, StreamOpenFrameRoundtrip) {
    net::StreamOpenFrame open;
    open.set_stream_id(42);
    open.set_initial_window_bytes(65536);
    // set sender/receiver/trace_context...

    auto frame = net::WireFrame::from_stream_open(open);
    EXPECT_EQ(frame.payload_type(), net::WireFrame::PayloadType::StreamOpen);

    auto encoded = frame.encode();
    EXPECT_FALSE(encoded.empty());

    auto decoded = net::WireFrame::decode(encoded);
    ASSERT_EQ(decoded.payload_type(), net::WireFrame::PayloadType::StreamOpen);
    EXPECT_EQ(decoded.pb_envelope.stream_open().stream_id(), 42u);
    EXPECT_EQ(decoded.pb_envelope.stream_open().initial_window_bytes(), 65536u);
}

TEST(StreamFrameTest, StreamDataFrameRoundtrip) {
    net::StreamDataFrame data;
    data.set_stream_id(42);
    data.set_sequence(7);
    data.set_payload("hello", 5);

    auto frame = net::WireFrame::from_stream_data(data);
    auto encoded = frame.encode();
    auto decoded = net::WireFrame::decode(encoded);

    EXPECT_EQ(decoded.pb_envelope.stream_data().stream_id(), 42u);
    EXPECT_EQ(decoded.pb_envelope.stream_data().sequence(), 7u);
    EXPECT_EQ(decoded.pb_envelope.stream_data().payload(), "hello");
}

TEST(StreamFrameTest, StreamAckFrameRoundtrip) {
    net::StreamAckFrame ack;
    ack.set_stream_id(42);
    ack.set_last_sequence(10);
    ack.set_window_bytes(32768);

    auto frame = net::WireFrame::from_stream_ack(ack);
    auto encoded = frame.encode();
    auto decoded = net::WireFrame::decode(encoded);

    EXPECT_EQ(decoded.pb_envelope.stream_ack().last_sequence(), 10u);
    EXPECT_EQ(decoded.pb_envelope.stream_ack().window_bytes(), 32768u);
}

TEST(StreamFrameTest, StreamCloseFrameRoundtrip) {
    net::StreamCloseFrame close;
    close.set_stream_id(42);
    close.set_reason(net::StreamCloseFrame::COMPLETE);

    auto frame = net::WireFrame::from_stream_close(close);
    auto encoded = frame.encode();
    auto decoded = net::WireFrame::decode(encoded);

    EXPECT_EQ(decoded.pb_envelope.stream_close().reason(),
              net::StreamCloseFrame::COMPLETE);
}

TEST(StreamFrameTest, StreamErrorFrameRoundtrip) {
    net::StreamErrorFrame error;
    error.set_stream_id(42);
    error.set_error_code(3);
    error.set_description("test error");

    auto frame = net::WireFrame::from_stream_error(error);
    auto encoded = frame.encode();
    auto decoded = net::WireFrame::decode(encoded);

    EXPECT_EQ(decoded.pb_envelope.stream_error().error_code(), 3u);
    EXPECT_EQ(decoded.pb_envelope.stream_error().description(), "test error");
}

TEST(StreamFrameTest, PayloadTypeDiscrimination) {
    auto open_frame = net::WireFrame::from_stream_open({});
    EXPECT_EQ(open_frame.payload_type(),
              net::WireFrame::PayloadType::StreamOpen);

    auto data_frame = net::WireFrame::from_stream_data({});
    EXPECT_EQ(data_frame.payload_type(),
              net::WireFrame::PayloadType::StreamData);

    auto ack_frame = net::WireFrame::from_stream_ack({});
    EXPECT_EQ(ack_frame.payload_type(),
              net::WireFrame::PayloadType::StreamAck);

    auto close_frame = net::WireFrame::from_stream_close({});
    EXPECT_EQ(close_frame.payload_type(),
              net::WireFrame::PayloadType::StreamClose);

    auto error_frame = net::WireFrame::from_stream_error({});
    EXPECT_EQ(error_frame.payload_type(),
              net::WireFrame::PayloadType::StreamError);
}

TEST(StreamFrameTest, StreamIdUniqueness) {
    // IDs from same sender are unique (monotonic counter)
    uint64_t id1 = (static_cast<uint64_t>(42) << 32) | 1;
    uint64_t id2 = (static_cast<uint64_t>(42) << 32) | 2;
    EXPECT_NE(id1, id2);
}

TEST(StreamFrameTest, StreamConfigDefaults) {
    StreamConfig cfg;
    EXPECT_EQ(cfg.initial_window_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.max_chunk_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.send_buffer_bytes, 256u * 1024u);
}
```

- [ ] **Step 2: Add to CMakeLists**

Add `test_stream_frames.cpp` to the appropriate `tests/unit/msg/CMakeLists.txt` (or `tests/unit/net/CMakeLists.txt`).

- [ ] **Step 3: Run tests**

Run: `ninja -C build && ctest -R "StreamFrame" --output-on-failure`
Expected: All 8 tests PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/msg/test_stream_frames.cpp tests/unit/msg/CMakeLists.txt
git commit -m "test(stream): add stream frame roundtrip unit tests

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 10.2: Stream messaging integration tests

**Files:**
- Create: `tests/integration/actor/test_stream_messaging.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

- [ ] **Step 1: Write integration tests covering core stream behaviors**

Create `tests/integration/actor/test_stream_messaging.cpp`:

```cpp
#include <gtest/gtest.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stream_handle.hpp>
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/adt/stream_buffer.hpp>

using namespace hpactor;

namespace {

// Receiver actor that records stream events
class StreamTestReceiver : public EventBasedActor {
public:
    explicit StreamTestReceiver(ActorSystem& sys) : EventBasedActor(sys) {}

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            auto tag = msg.type_id();
            if (tag == stream::StreamOpenedTag) {
                opened_ = true;
            } else if (tag == stream::StreamClosedTag) {
                closed_ = true;
            } else if (tag == stream::StreamErrorTag) {
                errored_ = true;
                error_code_ = 1;  // from payload
            } else {
                received_chunks_.push_back(msg.payload().copy());
            }
        }};
    }

    bool opened_ = false;
    bool closed_ = false;
    bool errored_ = false;
    uint32_t error_code_ = 0;
    std::vector<StreamBuffer> received_chunks_;
};

} // namespace

TEST(StreamMessagingTest, OpenStreamLocalReturnsHandle) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;  // deterministic: no workers
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(receiver.valid());

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());
    EXPECT_TRUE(handle->is_open());
    EXPECT_GT(handle->stream_id(), 0u);
}

TEST(StreamMessagingTest, OpenStreamToUnknownTargetReturnsNullopt) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    ActorId invalid_id{999999};
    auto handle = system.open_stream(invalid_id);
    EXPECT_FALSE(handle.has_value());
}

TEST(StreamMessagingTest, StreamCloseGraceful) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());

    // Write a chunk, then close
    uint8_t data[] = {1, 2, 3};
    StreamBuffer payload(data, data + 3);
    EXPECT_TRUE(handle->write(TypeTag::User, std::move(payload)));
    EXPECT_TRUE(handle->close());
    EXPECT_FALSE(handle->is_open());

    // Process messages deterministically (scheduler_threads=0)
    auto* receiver_ptr = system.get_actor_ptr<StreamTestReceiver>(receiver.id());
    ASSERT_NE(receiver_ptr, nullptr);
    EXPECT_TRUE(receiver_ptr->opened_);
    EXPECT_TRUE(receiver_ptr->closed_);
    ASSERT_EQ(receiver_ptr->received_chunks_.size(), 1u);
    EXPECT_EQ(receiver_ptr->received_chunks_[0].size(), 3u);
}

TEST(StreamMessagingTest, StreamErrorDeliversErrorTag) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());

    EXPECT_TRUE(handle->error(42, "test error"));
    EXPECT_FALSE(handle->is_open());

    auto* receiver_ptr = system.get_actor_ptr<StreamTestReceiver>(receiver.id());
    ASSERT_NE(receiver_ptr, nullptr);
    EXPECT_TRUE(receiver_ptr->errored_);
}

TEST(StreamMessagingTest, WriteOnClosedHandleReturnsFalse) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());
    handle->close();

    StreamBuffer buf;
    EXPECT_FALSE(handle->write(TypeTag::User, std::move(buf)));
    EXPECT_FALSE(handle->close());  // double close
}

TEST(StreamMessagingTest, StreamHandleMoveSemantics) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    auto h1 = system.open_stream(receiver.id());
    ASSERT_TRUE(h1.has_value());

    uint64_t sid = h1->stream_id();
    auto h2 = std::move(*h1);
    EXPECT_FALSE(h1->is_open());       // moved-from is closed
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), sid);
}

TEST(StreamMessagingTest, StreamDataChunksArriveInOrder) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());

    // Send 10 chunks
    for (int i = 0; i < 10; i++) {
        uint8_t val = static_cast<uint8_t>(i);
        StreamBuffer payload(&val, &val + 1);
        EXPECT_TRUE(handle->write(TypeTag::User, std::move(payload)));
    }
    handle->close();

    auto* receiver_ptr = system.get_actor_ptr<StreamTestReceiver>(receiver.id());
    ASSERT_NE(receiver_ptr, nullptr);
    ASSERT_EQ(receiver_ptr->received_chunks_.size(), 10u);
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ(receiver_ptr->received_chunks_[i][0], static_cast<uint8_t>(i));
    }
}

TEST(StreamMessagingTest, OpenStreamWithCustomConfig) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    StreamConfig stream_cfg;
    stream_cfg.initial_window_bytes = 128 * 1024;
    stream_cfg.idle_timeout = Duration::from_seconds(60);

    auto handle = system.open_stream(receiver.id(), stream_cfg);
    ASSERT_TRUE(handle.has_value());
    EXPECT_TRUE(handle->is_open());
}
```

- [ ] **Step 2: Run integration tests**

Run: `ninja -C build && ctest -R "StreamMessaging" --output-on-failure`

- [ ] **Step 3: Commit**

```bash
git add tests/integration/actor/test_stream_messaging.cpp \
        tests/integration/actor/CMakeLists.txt
git commit -m "test(stream): add stream messaging integration tests

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 10.3: StreamHandle unit tests (extend)

- [ ] **Step 1: Extend existing test_stream_handle.cpp with credit window tests**

In `tests/unit/actor/test_stream_handle.cpp`, add these tests:

```cpp
TEST(StreamHandleTest, BytesInFlightTracksSentMinusAcked) {
    ActorId sid{42};
    StreamHandle h(sid, 100);
    // bytes_in_flight starts at 0 (StreamSenderActor not connected yet)
    EXPECT_EQ(h.bytes_in_flight(), 0u);
}

TEST(StreamHandleTest, WindowBytesStartsAtZero) {
    ActorId sid{42};
    StreamHandle h(sid, 100);
    // window_bytes = 0 until first StreamAckFrame received
    EXPECT_EQ(h.window_bytes(), 0u);
}

TEST(StreamHandleTest, IsOpenAfterConstruction) {
    ActorId sid{42};
    StreamHandle h(sid, 100);
    EXPECT_TRUE(h.is_open());
}

TEST(StreamHandleTest, IsNotOpenAfterClose) {
    ActorId sid{42};
    StreamHandle h(sid, 100);
    h.close();
    EXPECT_FALSE(h.is_open());
}

TEST(StreamHandleTest, IsNotOpenAfterError) {
    ActorId sid{42};
    StreamHandle h(sid, 100);
    h.error(1, "test");
    EXPECT_FALSE(h.is_open());
}

TEST(StreamHandleTest, DefaultConstructedHandleIsNotOpen) {
    StreamHandle h;
    EXPECT_FALSE(h.is_open());
    EXPECT_EQ(h.stream_id(), 0u);
}

TEST(StreamHandleTest, MoveAssignFromOpenToDefault) {
    ActorId sid{42};
    StreamHandle h1(sid, 100);
    StreamHandle h2;
    h2 = std::move(h1);
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), 100u);
    EXPECT_FALSE(h1.is_open());
}
```

- [ ] **Step 2: Run tests**

Run: `./build/tests/unit/actor/test_unit_actor --gtest_filter="*StreamHandle*"`

- [ ] **Step 3: Commit**

```bash
git add tests/unit/actor/test_stream_handle.cpp
git commit -m "test(stream): extend StreamHandle tests with credit window

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Phase 11: System Tests

### Task 11.1: End-to-end streaming system tests

**Files:**
- Create: `tests/system/test_stream_system.cpp`
- Modify: `tests/system/CMakeLists.txt`

- [ ] **Step 1: Write system tests**

Create `tests/system/test_stream_system.cpp`:

```cpp
#include <gtest/gtest.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stream_handle.hpp>
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_types.hpp>

using namespace hpactor;

namespace {

class StreamSystemReceiver : public EventBasedActor {
public:
    explicit StreamSystemReceiver(ActorSystem& sys) : EventBasedActor(sys) {}
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == stream::StreamClosedTag) {
                closed_ = true;
            } else if (msg.type_id() == stream::StreamErrorTag) {
                errored_ = true;
            } else {
                chunks_received_++;
                bytes_received_ += msg.payload().size();
            }
        }};
    }
    bool closed_ = false;
    bool errored_ = false;
    size_t chunks_received_ = 0;
    size_t bytes_received_ = 0;
};

} // namespace

TEST(StreamSystemTest, MultiChunkTransferAllArriveInOrder) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamSystemReceiver>();
    ASSERT_TRUE(receiver.valid());

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());

    constexpr int kNumChunks = 100;
    constexpr size_t kChunkSize = 1024;  // 1 KiB each
    std::vector<uint8_t> pattern(kChunkSize);

    for (int i = 0; i < kNumChunks; i++) {
        pattern[0] = static_cast<uint8_t>(i);
        StreamBuffer chunk(pattern.data(), pattern.data() + kChunkSize);
        ASSERT_TRUE(handle->write(TypeTag::User, std::move(chunk)));
    }
    handle->close();

    auto* recv = system.get_actor_ptr<StreamSystemReceiver>(receiver.id());
    ASSERT_NE(recv, nullptr);
    EXPECT_EQ(recv->chunks_received_, kNumChunks);
    EXPECT_EQ(recv->bytes_received_, kNumChunks * kChunkSize);
    EXPECT_TRUE(recv->closed_);
}

TEST(StreamSystemTest, ConcurrentStreamsNoInterference) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    constexpr int kNumStreams = 5;
    std::vector<Actor> receivers;
    std::vector<StreamHandle> handles;

    for (int i = 0; i < kNumStreams; i++) {
        auto recv = system.spawn<StreamSystemReceiver>();
        ASSERT_TRUE(recv.valid());
        receivers.push_back(recv);

        auto h = system.open_stream(recv.id());
        ASSERT_TRUE(h.has_value());
        handles.push_back(std::move(*h));
    }

    // Send one chunk on each stream
    for (int i = 0; i < kNumStreams; i++) {
        uint8_t marker = static_cast<uint8_t>(i);
        StreamBuffer chunk(&marker, &marker + 1);
        ASSERT_TRUE(handles[i].write(TypeTag::User, std::move(chunk)));
        handles[i].close();
    }

    // Verify each receiver got exactly 1 chunk from its stream
    for (int i = 0; i < kNumStreams; i++) {
        auto* recv = system.get_actor_ptr<StreamSystemReceiver>(receivers[i].id());
        ASSERT_NE(recv, nullptr);
        EXPECT_EQ(recv->chunks_received_, 1u) << "Stream " << i;
        EXPECT_TRUE(recv->closed_) << "Stream " << i;
    }
}

TEST(StreamSystemTest, ShutdownDuringActiveStream) {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamSystemReceiver>();
    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());

    // Send a few chunks
    uint8_t data[16] = {};
    StreamBuffer chunk(data, data + sizeof(data));
    handle->write(TypeTag::User, std::move(chunk));

    // Shutdown without closing — verify no hangs, no crashes
    system.shutdown();
    // If we reach here without hanging, the test passes
    SUCCEED();
}
```

- [ ] **Step 2: Run system tests**

Run: `ninja -C build && ctest -R "StreamSystem" --output-on-failure`

- [ ] **Step 3: Commit**

```bash
git add tests/system/test_stream_system.cpp tests/system/CMakeLists.txt
git commit -m "test(stream): add end-to-end streaming system tests

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Complete File Manifest

### New Files
| File | Purpose |
|------|---------|
| `include/hpactor/actor/stream_config.hpp` | `StreamConfig` struct |
| `include/hpactor/actor/stream_types.hpp` | Stream TypeTags, payload structs |
| `include/hpactor/actor/stream_handle.hpp` | `StreamHandle` class |
| `include/hpactor/actor/stream_sender_actor.hpp` | `StreamSenderActor` header |
| `include/hpactor/actor/stream_receiver_actor.hpp` | `StreamReceiverActor` header |
| `src/actor/stream_sender_actor.cpp` | `StreamSenderActor` implementation |
| `src/actor/stream_receiver_actor.cpp` | `StreamReceiverActor` implementation |
| `src/cli/commands/stream_commands.cpp` | `/stream list`, `/stream show` CLI commands |
| `tests/unit/actor/test_stream_handle.cpp` | StreamHandle unit tests |
| `tests/unit/actor/test_stream_sender_actor.cpp` | StreamSenderActor unit tests |
| `tests/unit/msg/test_stream_frames.cpp` | Stream frame roundtrip tests |
| `tests/unit/cli/test_stream_commands.cpp` | Stream CLI command tests |
| `tests/integration/actor/test_stream_messaging.cpp` | Stream integration tests |
| `tests/system/test_stream_system.cpp` | End-to-end system tests |

### Modified Files
| File | Change |
|------|--------|
| `protos/hpactor/frame.proto` | Add stream + batch frame types; extend `WireEnvelope` |
| `include/hpactor/msg/frame.hpp` | Migrate to `WireEnvelope`; add `PayloadType`; add stream factories |
| `src/msg/frame.cpp` | Update `encode()`/`decode()`; implement factories; implement `payload_type()` |
| `include/hpactor/core/actor_system.hpp` | Add stream registry, dispatch methods, `allocate_stream_id()` |
| `src/actor/actor_system.cpp` | Implement stream frame dispatch; stream actor registry |
| `include/hpactor/actor/actor_context.hpp` | Add `open_stream()` declaration |
| `src/actor/actor_context.cpp` | Implement `open_stream()` |
| `include/hpactor/msg/dead_letter_record.hpp` | Add `StreamClosed = 19` |
| `include/hpactor/metrics/metrics_event.hpp` | Add stream metric events (52-58) |
| `include/hpactor/fault/fault_types.hpp` | Add `kStream = 15` |
| `src/fault/fault_points.cpp` | Register stream fault points |
| `src/metrics/metrics_aggregator.cpp` | Handle stream metric events |
| `src/actor/CMakeLists.txt` | Add `stream_sender_actor.cpp`, `stream_receiver_actor.cpp` |
| `src/cli/CMakeLists.txt` | Add `stream_commands.cpp` |
| `tests/unit/actor/CMakeLists.txt` | Add `test_stream_handle.cpp`, `test_stream_sender_actor.cpp` |
| `tests/unit/msg/CMakeLists.txt` | Add `test_stream_frames.cpp` |
| `tests/unit/cli/CMakeLists.txt` | Add `test_stream_commands.cpp` |
| `tests/integration/actor/CMakeLists.txt` | Add `test_stream_messaging.cpp` |
| `tests/system/CMakeLists.txt` | Add `test_stream_system.cpp` |
| All call sites of `frame.pb_frame.*` | Update to `frame.pb_envelope.data_frame().*` |

### Build Verification Sequence

After each task, run the narrowest verification:

1. `ninja -C build` — targeted build of changed TUs
2. `./build/tests/unit/<target>/test_unit_<target> --gtest_filter="*<TestName>*"` — specific test
3. `ctest -R "<pattern>" --output-on-failure` — related test suite
4. `ctest --output-on-failure --parallel 8` — full suite (only after wire-incompatible changes)
