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

#include <future>
#include <gtest/gtest.h>
#include <hpactor/msg/completion_port.hpp>
#include <hpactor/msg/delivery_receipt.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <thread>

using namespace hpactor;
using namespace hpactor::msg;
using namespace hpactor::mailbox;

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

} // namespace

TEST(DeliveryReceiptTest, DefaultConstructedNotReady) {
    DeliveryReceipt receipt;
    EXPECT_FALSE(receipt.ready());
}

TEST(DeliveryReceiptTest, FromImmediateResultReadyImmediately) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));
    EXPECT_TRUE(receipt.ready());
}

TEST(DeliveryReceiptTest, FromImmediateResultGetReturnsResult) {
    DeliveryResult result;
    result.status = DeliveryStatus::NoRoute;
    DeliveryReceipt receipt(std::move(result));
    EXPECT_TRUE(receipt.ready());
    auto got = receipt.get();
    EXPECT_EQ(got.status, DeliveryStatus::NoRoute);
}

TEST(DeliveryReceiptTest, FromImmediateResultTryGetReturnsValue) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));
    auto got = receipt.try_get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, MoveConstructedPreservesState) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt a(std::move(result));
    EXPECT_TRUE(a.ready());
    DeliveryReceipt b(std::move(a));
    EXPECT_TRUE(b.ready());
    auto got = b.get();
    EXPECT_EQ(got.status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, MoveAssignmentPreservesState) {
    DeliveryResult result;
    result.status = DeliveryStatus::ActorDead;
    DeliveryReceipt a(std::move(result));
    DeliveryReceipt b;
    b = std::move(a);
    EXPECT_TRUE(b.ready());
    EXPECT_EQ(b.get().status, DeliveryStatus::ActorDead);
}

TEST(DeliveryReceiptTest, GetBlocksUntilResolved) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));

    auto fut =
        std::async(std::launch::async, [&receipt]() { return receipt.get(); });
    auto got = fut.get();
    EXPECT_EQ(got.status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, MessageIdAccessor) {
    DeliveryResult result;
    result.message_id = MessageId{42};
    DeliveryReceipt receipt(std::move(result));
    EXPECT_EQ(receipt.message_id(), MessageId{42});
}

TEST(DeliveryReceiptTest, OnCompleteCallbackFiresSynchronously) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(std::move(result));

    bool called = false;
    DeliveryResult captured;
    receipt.on_complete([&](DeliveryResult r) {
        called = true;
        captured = r;
    });
    EXPECT_TRUE(called);
    EXPECT_EQ(captured.status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, CancelOnAlreadyResolvedIsNoop) {
    DeliveryResult result;
    result.status = DeliveryStatus::Accepted;
    result.message_id = MessageId{99};
    DeliveryReceipt receipt(std::move(result));
    receipt.cancel();
    EXPECT_TRUE(receipt.ready());
    EXPECT_EQ(receipt.get().status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, FixedPortFiresForAlreadyResolvedReceipt) {
    DeliveryResult delivered;
    delivered.status = DeliveryStatus::Accepted;
    DeliveryReceipt receipt(delivered);
    CompletionProbe<DeliveryResult> probe;

    ASSERT_TRUE(receipt.on_complete(CompletionPort<DeliveryResult>{
        &probe, &CompletionProbe<DeliveryResult>::complete, nullptr}));
    EXPECT_EQ(probe.calls, 1u);
    ASSERT_TRUE(probe.value.has_value());
    EXPECT_EQ(probe.value->status, DeliveryStatus::Accepted);
}

TEST(DeliveryReceiptTest, FixedPortCompletesExactlyOnce) {
    auto state = std::make_shared<DeliveryReceipt::SharedState>();
    DeliveryReceipt receipt(std::move(state));
    CompletionProbe<DeliveryResult> probe;

    ASSERT_TRUE(receipt.on_complete(CompletionPort<DeliveryResult>{
        &probe, &CompletionProbe<DeliveryResult>::complete, nullptr}));
    EXPECT_FALSE(receipt.on_complete(CompletionPort<DeliveryResult>{
        &probe, &CompletionProbe<DeliveryResult>::complete, nullptr}));

    DeliveryResult dr;
    dr.status = DeliveryStatus::Accepted;
    receipt.cancel(); // resolve via cancel
    EXPECT_EQ(probe.calls, 1u);
}

TEST(DeliveryReceiptTest, FixedPortRejectsWhenStdFunctionAlreadySet) {
    auto state = std::make_shared<DeliveryReceipt::SharedState>();
    DeliveryReceipt receipt(std::move(state));
    receipt.on_complete([](DeliveryResult) {});

    CompletionProbe<DeliveryResult> probe;
    EXPECT_FALSE(receipt.on_complete(CompletionPort<DeliveryResult>{
        &probe, &CompletionProbe<DeliveryResult>::complete, nullptr}));
    EXPECT_EQ(probe.calls, 0u);
}

TEST(DeliveryReceiptTest, StdFunctionRejectedWhenFixedPortAlreadySet) {
    auto state = std::make_shared<DeliveryReceipt::SharedState>();
    DeliveryReceipt receipt(std::move(state));
    CompletionProbe<DeliveryResult> probe;
    ASSERT_TRUE(receipt.on_complete(CompletionPort<DeliveryResult>{
        &probe, &CompletionProbe<DeliveryResult>::complete, nullptr}));

    bool called = false;
    receipt.on_complete([&](DeliveryResult) { called = true; });
    EXPECT_FALSE(called);
}
