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

// System test: Order Platform End-to-End
// Capstone test exercising the full-featured order platform example
// through its public message encode/decode API, verifying the full
// message flow: SubmitOrder → InventoryReserve → PaymentAuthorize →
// OrderFulfill, plus error scenarios.

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>

#include <apps/order_platform/messages.hpp>

#include "system_test_fixture.hpp"

#include <string>

using namespace hpactor;
using namespace hpactor::apps::order_platform;

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Happy path — all message types encode and decode correctly
// ═══════════════════════════════════════════════════════════════════════════════

TEST(OrderPlatform, OrderPlatformMessageRoundtrips) {
    // SubmitOrder
    {
        SubmitOrderPayload in;
        in.order_id = "ord-001";
        in.customer_id = "cust-alice";
        in.scenario = ScenarioKind::HappyPath;
        in.lines.push_back(OrderLine{"sku-A", 2, 2999});
        in.lines.push_back(OrderLine{"sku-B", 1, 1599});

        auto encoded = encode_submit_order(in);
        SubmitOrderPayload out;
        EXPECT_TRUE(decode_submit_order(encoded, out));
        EXPECT_EQ(out.order_id, "ord-001");
        EXPECT_EQ(out.customer_id, "cust-alice");
        EXPECT_EQ(out.scenario, ScenarioKind::HappyPath);
        EXPECT_EQ(out.lines.size(), 2u);
        EXPECT_EQ(out.lines[0].sku, "sku-A");
        EXPECT_EQ(out.lines[0].quantity, 2);
        EXPECT_EQ(out.lines[0].unit_cents, 2999);
        EXPECT_EQ(out.lines[1].sku, "sku-B");
        EXPECT_EQ(out.lines[1].quantity, 1);
        EXPECT_EQ(out.lines[1].unit_cents, 1599);
    }

    // OrderStatus
    {
        OrderStatusPayload in;
        in.order_id = "ord-002";
        in.status = OrderStatus::PaymentFailed;
        in.detail = "card_declined";
        in.total_cents = 4200;

        auto encoded = encode_order_status(in);
        OrderStatusPayload out;
        EXPECT_TRUE(decode_order_status(encoded, out));
        EXPECT_EQ(out.order_id, "ord-002");
        EXPECT_EQ(out.status, OrderStatus::PaymentFailed);
        EXPECT_EQ(out.detail, "card_declined");
    }

    // InventoryReserve
    {
        InventoryReservePayload in;
        in.order_id = "ord-003";
        in.lines.push_back(OrderLine{"sku-C", 3, 1000});

        auto encoded = encode_inventory_reserve(in);
        InventoryReservePayload out;
        EXPECT_TRUE(decode_inventory_reserve(encoded, out));
        EXPECT_EQ(out.order_id, "ord-003");
        EXPECT_EQ(out.lines.size(), 1u);
        EXPECT_EQ(out.lines[0].sku, "sku-C");
    }

    // PaymentAuthorize
    {
        PaymentAuthorizePayload in;
        in.order_id = "ord-004";
        in.customer_id = "cust-bob";
        in.amount_cents = 9900;
        in.scenario = ScenarioKind::HappyPath;

        auto encoded = encode_payment_authorize(in);
        PaymentAuthorizePayload out;
        EXPECT_TRUE(decode_payment_authorize(encoded, out));
        EXPECT_EQ(out.order_id, "ord-004");
        EXPECT_EQ(out.customer_id, "cust-bob");
        EXPECT_EQ(out.amount_cents, 9900);
    }

    // QueryOrder
    {
        QueryOrderPayload in;
        in.order_id = "ord-005";

        auto encoded = encode_query_order(in);
        QueryOrderPayload out;
        EXPECT_TRUE(decode_query_order(encoded, out));
        EXPECT_EQ(out.order_id, "ord-005");
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Failure scenarios — all ScenarioKind values parse correctly
// ═══════════════════════════════════════════════════════════════════════════════

TEST(OrderPlatform, AllFailureScenariosParse) {
    struct Case {
        const char* input;
        ScenarioKind expected;
    };
    Case cases[] = {
        {"happy-path", ScenarioKind::HappyPath},
        {"insufficient-stock", ScenarioKind::InsufficientStock},
        {"payment-decline", ScenarioKind::PaymentDecline},
        {"payment-timeout", ScenarioKind::PaymentTimeout},
        {"worker-crash", ScenarioKind::WorkerCrash},
        {"overload", ScenarioKind::Overload},
        {"missing-route", ScenarioKind::MissingRoute},
    };

    for (auto& tc : cases) {
        ScenarioKind s = scenario_from_string(tc.input);
        EXPECT_EQ(s, tc.expected);
    }

    // Unknown string defaults to HappyPath
    EXPECT_EQ(scenario_from_string("bogus-value"), ScenarioKind::HappyPath);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Bounded message encoding — consumer-side simulate a full order flow
// ═══════════════════════════════════════════════════════════════════════════════

TEST(OrderPlatform, SimulatedOrderFlow) {
    // Simulated order flow through encode/decode:
    // Coordinator → Inventory → Payment → Fulfillment

    // 1. Submit order (happy path)
    SubmitOrderPayload order;
    order.order_id = "sim-001";
    order.customer_id = "cust-z";
    order.scenario = ScenarioKind::HappyPath;
    order.lines.push_back(OrderLine{"sku-X", 1, 5000});

    auto encoded_order = encode_submit_order(order);

    SubmitOrderPayload decoded;
    EXPECT_TRUE(decode_submit_order(encoded_order, decoded));
    EXPECT_EQ(decoded.scenario, ScenarioKind::HappyPath);

    // 2. Inventory reserve
    InventoryReservePayload reserve;
    reserve.order_id = decoded.order_id;
    for (auto& line : decoded.lines) {
        reserve.lines.push_back(line);
    }
    auto encoded_reserve = encode_inventory_reserve(reserve);
    InventoryReservePayload decoded_reserve;
    EXPECT_TRUE(decode_inventory_reserve(encoded_reserve, decoded_reserve));
    EXPECT_EQ(decoded_reserve.order_id, "sim-001");
    EXPECT_EQ(decoded_reserve.lines.size(), 1u);

    // 3. Payment authorize
    PaymentAuthorizePayload payment;
    payment.order_id = decoded_reserve.order_id;
    payment.customer_id = decoded.customer_id;
    payment.amount_cents = 5000;
    payment.scenario = ScenarioKind::HappyPath;

    auto encoded_payment = encode_payment_authorize(payment);
    PaymentAuthorizePayload decoded_payment;
    EXPECT_TRUE(decode_payment_authorize(encoded_payment, decoded_payment));
    EXPECT_EQ(decoded_payment.amount_cents, 5000);

    // 4. Order fulfilled status
    OrderStatusPayload fulfilled;
    fulfilled.order_id = decoded_payment.order_id;
    fulfilled.status = OrderStatus::Completed;
    fulfilled.total_cents = decoded_payment.amount_cents;

    auto encoded_status = encode_order_status(fulfilled);
    OrderStatusPayload decoded_status;
    EXPECT_TRUE(decode_order_status(encoded_status, decoded_status));
    EXPECT_EQ(decoded_status.status, OrderStatus::Completed);
    EXPECT_EQ(decoded_status.order_id, "sim-001");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: Malformed payloads are rejected
// ═══════════════════════════════════════════════════════════════════════════════

TEST(OrderPlatform, MalformedPayloadsRejected) {
    // SubmitOrder with truncated payload
    {
        StreamBuffer truncated{0x00, 0x00, 0x00, 0x04, 'o'};
        SubmitOrderPayload out;
        EXPECT_FALSE(decode_submit_order(truncated, out));
    }

    // QueryOrder with truncated payload
    {
        StreamBuffer truncated{0x00, 0x00, 0x00, 0x02, 'x'};
        QueryOrderPayload out;
        EXPECT_FALSE(decode_query_order(truncated, out));
    }

    // InventoryReserve with empty payload
    {
        StreamBuffer empty;
        InventoryReservePayload out;
        EXPECT_FALSE(decode_inventory_reserve(empty, out));
    }

    // PaymentAuthorize with garbage
    {
        StreamBuffer garbage{0xFF, 0xFF, 0xFF, 0xFF, 0x00};
        PaymentAuthorizePayload out;
        EXPECT_FALSE(decode_payment_authorize(garbage, out));
    }

    // OrderStatus with truncated payload
    {
        StreamBuffer truncated{0x00, 0x00, 0x00, 0x08, 'o', 'r', 'd'};
        OrderStatusPayload out;
        EXPECT_FALSE(decode_order_status(truncated, out));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: OrderStatus to_string conversion
// ═══════════════════════════════════════════════════════════════════════════════

TEST(OrderPlatform, OrderStatusToString) {
    EXPECT_EQ(to_string(OrderStatus::Received), std::string("received"));
    EXPECT_EQ(to_string(OrderStatus::InventoryReserved), std::string("inventory"
                                                                     "_reserve"
                                                                     "d"));
    EXPECT_EQ(to_string(OrderStatus::PaymentAuthorized), std::string("payment_"
                                                                     "authorize"
                                                                     "d"));
    EXPECT_EQ(to_string(OrderStatus::Completed), std::string("completed"));
    EXPECT_EQ(to_string(OrderStatus::PaymentFailed), std::string("payment_"
                                                                 "failed"));
    EXPECT_EQ(to_string(OrderStatus::PaymentTimedOut), std::string("payment_"
                                                                   "timed_"
                                                                   "out"));
    EXPECT_EQ(to_string(OrderStatus::Overloaded), std::string("overloaded"));
}