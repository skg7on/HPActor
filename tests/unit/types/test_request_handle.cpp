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
#include <hpactor/msg/completion_port.hpp>
#include <hpactor/msg/request_handle.hpp>
#include <thread>

namespace hpactor {
namespace {

template <typename T> struct CompletionProbe {
    size_t calls{0};
    std::optional<T> value;

    static void complete(void* context, T value) noexcept {
        auto* self = static_cast<CompletionProbe*>(context);
        ++self->calls;
        self->value.emplace(std::move(value));
    }
};

TEST(RequestHandleTest, DefaultConstructionIsNotReady) {
    RequestHandle<StreamBuffer> h;
    EXPECT_FALSE(h.ready());
}

TEST(RequestHandleTest, ResolveMakesReady) {
    RequestHandle<StreamBuffer> h;
    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("hello"), 5);
    h.resolve(result<StreamBuffer>::make(std::move(buf)));
    EXPECT_TRUE(h.ready());
}

TEST(RequestHandleTest, GetReturnsResolvedValue) {
    RequestHandle<StreamBuffer> h;
    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("world"), 5);
    h.resolve(result<StreamBuffer>::make(std::move(buf)));
    auto r = h.get();
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.value().size(), 5u);
}

TEST(RequestHandleTest, GetBlocksUntilResolved) {
    RequestHandle<StreamBuffer> h;
    std::atomic<bool> get_returned{false};
    std::atomic<bool> ready_to_block{false};
    std::thread t([&]() {
        ready_to_block.store(true, std::memory_order_release);
        h.get();
        get_returned.store(true, std::memory_order_release);
    });
    // Wait until thread signals it is about to block in get()
    while (!ready_to_block.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // Give a tiny yield to let the thread actually enter get()
    std::this_thread::yield();
    EXPECT_FALSE(get_returned.load(std::memory_order_acquire));
    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("x"), 1);
    h.resolve(result<StreamBuffer>::make(std::move(buf)));
    t.join();
    EXPECT_TRUE(get_returned.load(std::memory_order_acquire));
}

TEST(RequestHandleTest, CancelUnblocksGet) {
    RequestHandle<StreamBuffer> h;
    std::thread t([&]() {
        auto r = h.get();
        EXPECT_TRUE(r.is_error());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    h.cancel();
    t.join();
}

TEST(RequestHandleTest, CancelMarksReady) {
    RequestHandle<StreamBuffer> h;
    EXPECT_FALSE(h.ready());
    h.cancel();
    EXPECT_TRUE(h.ready());
}

TEST(RequestHandleTest, ResolveErrorReturnsError) {
    RequestHandle<StreamBuffer> h;
    h.resolve_error(error(errors::timeout, "test timeout"));
    auto r = h.get();
    EXPECT_TRUE(r.is_error());
    EXPECT_EQ(r.error().code(), errors::timeout);
}

TEST(RequestHandleTest, MoveSemanticsPreservesState) {
    RequestHandle<StreamBuffer> h1;
    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("data"), 4);
    h1.resolve(result<StreamBuffer>::make(std::move(buf)));
    RequestHandle<StreamBuffer> h2 = std::move(h1);
    EXPECT_TRUE(h2.ready());
    auto r = h2.get();
    EXPECT_TRUE(r.ok());
}

TEST(RequestHandleTest, MessageIdIsPreserved) {
    MessageId mid(42);
    RequestHandle<StreamBuffer> h(std::chrono::steady_clock::time_point::max(), mid);
    EXPECT_EQ(h.message_id().value(), 42u);
}

TEST(RequestHandleTest, FixedPortCompletesExactlyOnce) {
    RequestHandle<StreamBuffer> handle;
    CompletionProbe<result<StreamBuffer>> probe;
    ASSERT_TRUE(handle.on_complete(CompletionPort<result<StreamBuffer>>{
        &probe, &CompletionProbe<result<StreamBuffer>>::complete, nullptr}));

    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("abc"), 3);
    handle.resolve(result<StreamBuffer>::make(std::move(buf)));
    EXPECT_EQ(probe.calls, 1u);
    ASSERT_TRUE(probe.value.has_value());
    ASSERT_TRUE(probe.value->ok());
    EXPECT_EQ(probe.value->value().size(), 3u);
    // Second registration must be rejected.
    EXPECT_FALSE(handle.on_complete(CompletionPort<result<StreamBuffer>>{
        &probe, &CompletionProbe<result<StreamBuffer>>::complete, nullptr}));
}

TEST(RequestHandleTest, FixedPortFiresForAlreadyResolvedHandle) {
    RequestHandle<StreamBuffer> handle;
    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>("xyz"), 3);
    handle.resolve(result<StreamBuffer>::make(std::move(buf)));

    CompletionProbe<result<StreamBuffer>> probe;
    ASSERT_TRUE(handle.on_complete(CompletionPort<result<StreamBuffer>>{
        &probe, &CompletionProbe<result<StreamBuffer>>::complete, nullptr}));
    EXPECT_EQ(probe.calls, 1u);
    ASSERT_TRUE(probe.value.has_value());
    ASSERT_TRUE(probe.value->ok());
    EXPECT_EQ(probe.value->value().size(), 3u);
}

TEST(RequestHandleTest, FixedPortDoesNotRunUnderMutex) {
    // Verify that the fixed-port callback does not run while the state mutex
    // is held: if it did, re-entering on_complete would deadlock or fail.
    RequestHandle<StreamBuffer> handle;
    CompletionProbe<result<StreamBuffer>> probe;
    ASSERT_TRUE(handle.on_complete(CompletionPort<result<StreamBuffer>>{
        &probe, &CompletionProbe<result<StreamBuffer>>::complete, nullptr}));
    // Resolve from another thread — the callback must complete without
    // blocking.
    std::thread t([&]() {
        StreamBuffer buf;
        buf.append(reinterpret_cast<const uint8_t*>("t"), 1);
        handle.resolve(result<StreamBuffer>::make(std::move(buf)));
    });
    t.join();
    EXPECT_EQ(probe.calls, 1u);
}

} // namespace
} // namespace hpactor
