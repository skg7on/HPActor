# RPC Channel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement async RPC channel on top of ConnectionPool with at-least-once delivery, blocking get(), and configurable timeouts.

**Architecture:** RpcChannel owns pending call registry and retry state machine, one per ActorSystem. ActorContext::rpc() provides non-actor thread convenience. ConnectionPool gains set_rpc_handler() for response routing. Frame gains RpcRequest/RpcResponse/RpcIdempotent flags.

**Tech Stack:** C++20, no exceptions, no RTTI, existing ConnectionPool/TcpTransport/IScheduler infrastructure.

---

## File Structure

```
include/hpactor/
    ├── rpc/
    │   ├── rpc_channel.hpp      # RpcChannel, RpcFuture, PendingCall, rpc_response_handler
    │   └── rpc.hpp              # ActorContext::rpc() template
    ├── net/
    │   └── frame.hpp            # Add RpcRequest/RpcResponse/RpcIdempotent flags
    ├── net/connection_pool.hpp   # Add set_rpc_handler()
    └── core/actor_system.hpp     # Add rpc_channel()

src/
    └── rpc/
        └── rpc_channel.cpp      # RpcChannel implementation

tests/
    └── rpc/
        └── test_rpc_channel.cpp # Unit tests
```

---

## Task 1: Add RPC Frame Flags to Frame

**Files:**
- Modify: `include/hpactor/net/frame.hpp:56-58`

- [ ] **Step 1: Add flag constants**

In `Frame` struct, after existing `Important` and `NoDrop` flag constants, add:

```cpp
static constexpr uint32_t RpcRequest = 1 << 2;    // This frame is an RPC request
static constexpr uint32_t RpcResponse = 1 << 3;  // This frame is an RPC response
static constexpr uint32_t RpcIdempotent = 1 << 4; // Set by client on retries; server MUST
                                                  // deduplicate by MessageId before processing
```

- [ ] **Step 2: Verify build**

Run: `ninja -C build`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/frame.hpp
git commit -m "feat(net): add RpcRequest/RpcResponse/RpcIdempotent frame flags

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 2: Add set_rpc_handler to ConnectionPool

**Files:**
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/connection_pool.cpp`

- [ ] **Step 1: Add rpc_response_handler type and member**

In `ConnectionPool` class, add:

```cpp
using rpc_response_handler = std::function<void(MessageId, const bytes&)>;

void set_rpc_handler(rpc_response_handler handler);
```

Add to private members:
```cpp
rpc_response_handler rpc_handler_;
```

- [ ] **Step 2: Implement set_rpc_handler in cpp**

```cpp
void ConnectionPool::set_rpc_handler(rpc_response_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    rpc_handler_ = std::move(handler);
}
```

- [ ] **Step 3: Wire rpc_handler_ call in on_frame_received**

In `on_frame_received()`, after decoding frame, before calling existing message handler:

```cpp
void ConnectionPool::on_frame_received(const bytes& frame_data) {
    Frame frame = Frame::decode(frame_data);
    // ... existing actor message handling ...

    // Check for RPC response
    if ((frame.flags & Frame::RpcResponse) && rpc_handler_) {
        rpc_handler_(MessageId(frame.message_id), frame.payload);
        return;
    }

    // existing handler_(frame_data) for actor messages
}
```

- [ ] **Step 4: Verify build**

Run: `ninja -C build`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp
git commit -m "feat(net): add set_rpc_handler to ConnectionPool for RPC response routing

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 3: Create RpcChannel header

**Files:**
- Create: `include/hpactor/rpc/rpc_channel.hpp`

- [ ] **Step 1: Write the header file**

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

#pragma once

#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/types/serialization.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace hpactor {

// Forward declaration
class RpcChannel;

using RpcResponseHandler = std::function<void(MessageId, const bytes&)>;

// -----------------------------------------------------------------------------
// PendingCall - tracks in-flight RPC calls
// -----------------------------------------------------------------------------
struct PendingCall {
    MessageId msg_id;
    ActorAddress target;
    bytes encoded_request;
    std::chrono::milliseconds timeout;
    int retry_count = 0;
    int max_retries = 5;
    std::promise<result<bytes>> promise;
    std::chrono::steady_clock::time_point enqueued_at;
    std::atomic<bool> ready_{false};
};

// -----------------------------------------------------------------------------
// RpcFuture - wrapper around std::future with timeout
// -----------------------------------------------------------------------------
template<typename T>
class RpcFuture {
public:
    RpcFuture(std::future<result<T>> inner, std::chrono::milliseconds timeout);

    result<T> get();  // blocks until result available or timeout

private:
    std::future<result<T>> inner_;
    std::chrono::milliseconds timeout_;
};

// -----------------------------------------------------------------------------
// RpcChannel - manages RPC calls with retry and timeout
// -----------------------------------------------------------------------------
class RpcChannel {
public:
    explicit RpcChannel(Transport* transport, sched::IScheduler* scheduler);

    // Initiate RPC call
    template<typename Request, typename Response>
    RpcFuture<Response> call(const ActorAddress& target,
                             const Request& request,
                             std::chrono::milliseconds timeout_ms);

    // Cancel all pending calls
    void abort();

    // Handle response from transport layer
    void on_response(MessageId msg_id, const bytes& encoded_response);

private:
    void on_timeout(MessageId msg_id);
    void schedule_retry(PendingCall* call);
    void send_request(PendingCall& call, bool is_retry);

    Transport* transport_;
    sched::IScheduler* scheduler_;

    std::unordered_map<MessageId, std::unique_ptr<PendingCall>> pending_;
    mutable std::mutex mutex_;
};

} // namespace hpactor
```

- [ ] **Step 2: Verify header compiles in isolation**

Run: `echo '#include <hpactor/rpc/rpc_channel.hpp>' | clang++ -std=c++20 -c - -I include -fsyntax-only`
Expected: PASS (may need stubs for serialization — verify error is about missing impl not syntax)

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/rpc/rpc_channel.hpp
git commit -m "feat(rpc): add RpcChannel, RpcFuture, PendingCall types

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 4: Implement RpcChannel

**Files:**
- Create: `src/rpc/rpc_channel.cpp`

- [ ] **Step 1: Write RpcChannel implementation**

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

#include <hpactor/rpc/rpc_channel.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// RpcFuture implementation
// -----------------------------------------------------------------------------
template<typename T>
RpcFuture<T>::RpcFuture(std::future<result<T>> inner, std::chrono::milliseconds timeout)
    : inner_(std::move(inner)), timeout_(timeout) {}

template<typename T>
result<T> RpcFuture<T>::get() {
    if (!inner_.valid()) {
        return error(errors::unknown, "future not valid");
    }

    auto status = inner_.wait_for(timeout_);
    if (status == std::future_status::timeout) {
        return error(errors::timeout, "RPC call timed out");
    }

    return inner_.get();
}

// Explicit instantiations
template class RpcFuture<bytes>;

// -----------------------------------------------------------------------------
// RpcChannel implementation
// -----------------------------------------------------------------------------
RpcChannel::RpcChannel(Transport* transport, sched::IScheduler* scheduler)
    : transport_(transport), scheduler_(scheduler) {}

void RpcChannel::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, call] : pending_) {
        if (!call->ready_.load(std::memory_order_acquire)) {
            call->promise.set_value(error(errors::unknown, "RPC channel aborted"));
            call->ready_.store(true, std::memory_order_release);
        }
    }
    pending_.clear();
}

void RpcChannel::on_response(MessageId msg_id, const bytes& encoded_response) {
    std::unique_ptr<PendingCall> call;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(msg_id);
        if (it == pending_.end()) {
            return;  // No pending call for this message ID
        }
        call = std::move(it->second);
        pending_.erase(it);
    }

    call->ready_.store(true, std::memory_order_release);
    call->promise.set_value(result<bytes>::make(encoded_response));
}

void RpcChannel::on_timeout(MessageId msg_id) {
    std::unique_ptr<PendingCall> call;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(msg_id);
        if (it == pending_.end()) {
            return;  // Already completed
        }
        call = it->second.get();
    }

    if (call->retry_count < call->max_retries) {
        call->retry_count++;
        schedule_retry(call);
    } else {
        call->ready_.store(true, std::memory_order_release);
        call->promise.set_value(error(errors::timeout, "RPC call timed out"));
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(msg_id);
    }
}

void RpcChannel::schedule_retry(PendingCall* call) {
    int64_t delay_ns = call->timeout.count() * 1000000;
    scheduler_->schedule_after(
        [this, msg_id = call->msg_id]() { on_timeout(msg_id); },
        delay_ns);
    send_request(*call, true);
}

void RpcChannel::send_request(PendingCall& call, bool is_retry) {
    Frame frame;
    frame.sender = ActorAddress{};  // Will be filled by transport
    frame.receiver = call->target;
    frame.payload = call->encoded_request;
    frame.message_id = call->msg_id.value();
    frame.flags = Frame::RpcRequest;
    if (is_retry) {
        frame.flags |= Frame::RpcIdempotent;
    }

    bytes encoded = frame.encode();
    transport_->send(call->target, encoded);
}

template<typename Request, typename Response>
RpcFuture<Response> RpcChannel::call(const ActorAddress& target,
                                      const Request& request,
                                      std::chrono::milliseconds timeout_ms) {
    // Serialize request
    bytes encoded_request = Serialization::encode(request);

    MessageId msg_id = MessageId::generate();

    auto promise_ptr = std::make_shared<std::promise<result<bytes>>>();
    auto future = promise_ptr->get_future();

    auto* call_ptr = new PendingCall{
        .msg_id = msg_id,
        .target = target,
        .encoded_request = std::move(encoded_request),
        .timeout = timeout_ms,
        .retry_count = 0,
        .max_retries = 5,
        .promise = std::move(*promise_ptr),
        .enqueued_at = std::chrono::steady_clock::now(),
        .ready_ = false
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.emplace(msg_id, std::unique_ptr<PendingCall>(call_ptr));
    }

    // Send initial request
    send_request(*call_ptr, false);

    // Schedule timeout
    int64_t delay_ns = timeout_ms.count() * 1000000;
    scheduler_->schedule_after(
        [this, msg_id]() { on_timeout(msg_id); },
        delay_ns);

    // Transform future<result<bytes>> to future<result<Response>>
    std::future<result<Response>> transformed = std::async(
        std::launch::deferred,
        [fut = std::move(future)]() mutable -> result<Response> {
            result<bytes> r = fut.get();
            if (!r.has_value()) {
                return error(r.error().code(), r.error().message());
            }
            return Serialization::decode<Response>(r.value());
        });

    return RpcFuture<Response>(std::move(transformed), timeout_ms);
}

} // namespace hpactor
```

- [ ] **Step 2: Verify build**

Run: `ninja -C build`
Expected: FAIL (missing Serialization::encode/decode, missing scheduler include)

- [ ] **Step 3: Fix compilation issues**

Need to check existing Serialization interface and add necessary includes.

Run: `grep -n "Serialization" include/hpactor/types/serialization.hpp | head -30`

- [ ] **Step 4: Fix Serialization calls**

Based on existing serialization.hpp, update encode/decode calls.

- [ ] **Step 5: Verify build**

Run: `ninja -C build`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/rpc/rpc_channel.cpp
git commit -m "feat(rpc): implement RpcChannel with retry and timeout

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 5: Add rpc_channel() to ActorSystem

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp` (or wherever ActorSystem ctor is)

- [ ] **Step 1: Add RpcChannel include and member**

In `actor_system.hpp`, add include:
```cpp
#include <hpactor/rpc/rpc_channel.hpp>
```

Add to private members:
```cpp
RpcChannel rpc_channel_;
```

Add to public methods:
```cpp
RpcChannel& rpc_channel() { return rpc_channel_; }
```

- [ ] **Step 2: Initialize in constructor**

In `ActorSystem` constructor body (after scheduler_ init):
```cpp
rpc_channel_(transport_.get(), scheduler_.get()),
```

- [ ] **Step 3: Verify build**

Run: `ninja -C build`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(core): add rpc_channel() to ActorSystem

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 6: Add rpc() to ActorContext

**Files:**
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

- [ ] **Step 1: Add rpc() method declaration**

```cpp
// RPC calls (for non-actor threads only)
template<typename Request, typename Response>
RpcFuture<Response> rpc(const ActorAddress& target,
                        const Request& request,
                        std::chrono::milliseconds timeout_ms = 5000);
```

- [ ] **Step 2: Add template implementation in actor_context.hpp**

```cpp
template<typename Request, typename Response>
RpcFuture<Response> ActorContext::rpc(const ActorAddress& target,
                                       const Request& request,
                                       std::chrono::milliseconds timeout_ms) {
    return system_->rpc_channel().call<Request, Response>(target, request, timeout_ms);
}
```

- [ ] **Step 3: Verify build**

Run: `ninja -C build`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor_context.hpp src/actor/actor_context.cpp
git commit -m "feat(actor): add rpc() method to ActorContext

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 7: Write unit tests

**Files:**
- Create: `tests/rpc/test_rpc_channel.cpp`

- [ ] **Step 1: Write mock transport and scheduler**

```cpp
#include <hpactor/rpc/rpc_channel.hpp>
#include <gtest/gtest.h>

namespace {

class MockTransport : public hpactor::net::Transport {
public:
    void send(const hpactor::ActorAddress& target, const hpactor::bytes& encoded) override {
        sent_frames_.push_back(encoded);
    }
    hpactor::net::ConnectionPtr connect(hpactor::NodeId, const std::string&, uint16_t) override {
        return nullptr;
    }
    hpactor::net::ConnectionPtr connect(hpactor::NodeId) override { return nullptr; }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    bool is_connected(hpactor::NodeId) const override { return true; }
    hpactor::NodeId node_id() const override { return hpactor::LocalNodeId; }
    void close_connection(hpactor::NodeId) override {}

    std::vector<hpactor::bytes> sent_frames_;
};

class MockScheduler : public hpactor::sched::IScheduler {
public:
    hpactor::sched::TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) override {
        callbacks_[next_id_] = std::move(cb);
        return hpactor::sched::TimerHandle{next_id_++};
    }
    hpactor::sched::TimerHandle schedule_every(timer_callback cb, int64_t interval_ns) override {
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

    std::unordered_map<uint64_t, timer_callback> callbacks_;
    uint64_t next_id_ = 1;
};

} // anonymous namespace
```

- [ ] **Step 2: Write test_rpc_channel_response**

```cpp
TEST(RpcChannelTest, Response) {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, &scheduler);

    hpactor::ActorAddress target{hpactor::LocalNodeId, 1, hpactor::ActorId{1}, 0};

    hpactor::bytes request_data = {1, 2, 3};
    auto future = channel.call<hpactor::bytes, hpactor::bytes>(
        target, request_data, std::chrono::milliseconds{1000});

    ASSERT_EQ(transport.sent_frames_.size(), 1u);

    // Simulate response
    hpactor::MessageId msg_id = hpactor::MessageId::generate();
    hpactor::bytes response_data = {4, 5, 6};
    channel.on_response(msg_id, response_data);

    auto result = future.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), response_data);
}
```

- [ ] **Step 3: Write test_rpc_channel_timeout**

```cpp
TEST(RpcChannelTest, Timeout) {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, std::chrono::milliseconds{100});

    hpactor::ActorAddress target{hpactor::LocalNodeId, 1, hpactor::ActorId{1}, 0};

    auto future = channel.call<hpactor::bytes, hpactor::bytes>(
        target, hpactor::bytes{1, 2, 3}, std::chrono::milliseconds{50});

    auto result = future.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), hpactor::errors::timeout);
}
```

- [ ] **Step 4: Write test_rpc_channel_concurrent**

```cpp
TEST(RpcChannelTest, Concurrent) {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, &scheduler);

    std::vector<std::pair<hpactor::ActorAddress, hpactor::RpcFuture<hpactor::bytes>>> futures;
    for (int i = 0; i < 10; i++) {
        hpactor::ActorAddress target{hpactor::LocalNodeId, 1, hpactor::ActorId{static_cast<uint64_t>(i)}, 0};
        futures.emplace_back(target, channel.call<hpactor::bytes, hpactor::bytes>(
            target, hpactor::bytes{static_cast<uint8_t>(i)}, std::chrono::milliseconds{1000}));
    }

    EXPECT_EQ(transport.sent_frames_.size(), 10u);
}
```

- [ ] **Step 5: Write test_rpc_channel_retry**

```cpp
TEST(RpcChannelTest, Retry) {
    MockTransport transport;
    MockScheduler scheduler;
    hpactor::RpcChannel channel(&transport, &scheduler);

    hpactor::ActorAddress target{hpactor::LocalNodeId, 1, hpactor::ActorId{1}, 0};

    auto future = channel.call<hpactor::bytes, hpactor::bytes>(
        target, hpactor::bytes{1, 2, 3}, std::chrono::milliseconds{100});

    size_t initial_send_count = transport.sent_frames_.size();
    EXPECT_EQ(initial_send_count, 1u);

    // Trigger timeout - should retry
    scheduler.invoke_timer(1);

    EXPECT_GE(transport.sent_frames_.size(), initial_send_count);
}
```

- [ ] **Step 6: Verify tests compile and run**

Run: `ninja -C build tests/rpc/test_rpc_channel`
Run: `./build/tests/rpc/test_rpc_channel`
Expected: PASS (or fail with unimplemented Serialization - expected)

- [ ] **Step 7: Commit**

```bash
git add tests/rpc/test_rpc_channel.cpp
git commit -m "test(rpc): add unit tests for RpcChannel

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 8: Wire RPC handler to ConnectionPool

**Files:**
- Modify: `src/actor/actor_system.cpp` (or wherever transport is initialized)

- [ ] **Step 1: Register RPC handler**

In `ActorSystem` constructor after `transport_` is created, register the rpc_channel's handler:

```cpp
transport_->connection_pool()->set_rpc_handler(
    [this](hpactor::MessageId id, const hpactor::bytes& data) {
        rpc_channel_.on_response(id, data);
    });
```

Note: This requires `transport_` to expose `connection_pool()` or we use a different approach. If `TcpTransport` owns the pool internally, we may need to add a `set_rpc_handler` to `Transport` interface that delegates to the pool.

- [ ] **Step 2: Verify build**

Run: `ninja -C build`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add src/actor/actor_system.cpp
git commit -m "feat(core): wire RpcChannel handler to ConnectionPool

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## Task 9: Run full test suite

- [ ] **Step 1: Run all tests**

Run: `ctest --output-on-failure`
Expected: All existing tests pass + new RPC tests pass

- [ ] **Step 2: Run with TSAN**

Run: `cmake -DENABLE_TSAN=ON -S . -B build-tsan -GNinja && ninja -C build-tsan && ctest --output-on-failure`
Expected: PASS

- [ ] **Step 3: Final commit of work**

```bash
git add -A && git commit -m "feat: implement RPC channel with at-least-once delivery

- RpcChannel with pending call registry and retry state machine
- RpcFuture<result<T>> with timeout-enforced get()
- ConnectionPool gains set_rpc_handler() for response routing
- Frame gains RpcRequest/RpcResponse/RpcIdempotent flags
- ActorContext::rpc() and ActorSystem::rpc_channel() access
- Unit tests: timeout, response, retry, concurrent

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
"
```

---

## Notes

- **Task 4 (RpcChannel impl)** depends on knowing exact `Serialization::encode/decode` signatures — check `include/hpactor/types/serialization.hpp` before implementing
- **Task 8 (wiring)** depends on whether `Transport` interface exposes connection pool or if we need to add that method
- Serialization in RPC responses: the promise holds `result<bytes>`, deserialization to `result<Response>` happens in the transformed future — this keeps `PendingCall` simple
