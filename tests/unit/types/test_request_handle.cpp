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
#include <hpactor/msg/request_handle.hpp>
#include <thread>

namespace hpactor {
namespace {

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

} // namespace
} // namespace hpactor
