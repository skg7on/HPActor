// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <examples/order_platform/messages.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hpactor::examples::order_platform;

TEST(OrderPlatformMessagesTest, SubmitOrderRoundTrip) {
    SubmitOrderPayload in;
    in.order_id = "ord-100";
    in.customer_id = "cust-7";
    in.scenario = ScenarioKind::HappyPath;
    in.lines.push_back(OrderLine{"sku-book", 2, 1599});
    in.lines.push_back(OrderLine{"sku-pen", 3, 250});

    auto encoded = encode_submit_order(in);
    SubmitOrderPayload out;
    ASSERT_TRUE(decode_submit_order(encoded, out));
    EXPECT_EQ(out.order_id, "ord-100");
    EXPECT_EQ(out.customer_id, "cust-7");
    EXPECT_EQ(out.scenario, ScenarioKind::HappyPath);
    ASSERT_EQ(out.lines.size(), 2u);
    EXPECT_EQ(out.lines[0].sku, "sku-book");
    EXPECT_EQ(out.lines[0].quantity, 2);
    EXPECT_EQ(out.lines[0].unit_cents, 1599);
    EXPECT_EQ(out.lines[1].sku, "sku-pen");
    EXPECT_EQ(out.lines[1].quantity, 3);
    EXPECT_EQ(out.lines[1].unit_cents, 250);
}

TEST(OrderPlatformMessagesTest, StatusRoundTrip) {
    OrderStatusPayload in;
    in.order_id = "ord-200";
    in.status = OrderStatus::PaymentFailed;
    in.detail = "card declined";
    in.total_cents = 4242;

    auto encoded = encode_order_status(in);
    OrderStatusPayload out;
    ASSERT_TRUE(decode_order_status(encoded, out));
    EXPECT_EQ(out.order_id, "ord-200");
    EXPECT_EQ(out.status, OrderStatus::PaymentFailed);
    EXPECT_EQ(out.detail, "card declined");
    EXPECT_EQ(out.total_cents, 4242);
    EXPECT_EQ(to_string(out.status), std::string("payment_failed"));
}

TEST(OrderPlatformMessagesTest, InventoryRoundTrip) {
    InventoryReservePayload in;
    in.order_id = "ord-300";
    in.lines.push_back(OrderLine{"sku-lamp", 1, 3200});

    auto encoded = encode_inventory_reserve(in);
    InventoryReservePayload out;
    ASSERT_TRUE(decode_inventory_reserve(encoded, out));
    EXPECT_EQ(out.order_id, "ord-300");
    ASSERT_EQ(out.lines.size(), 1u);
    EXPECT_EQ(out.lines[0].sku, "sku-lamp");
}

TEST(OrderPlatformMessagesTest, PaymentRoundTrip) {
    PaymentAuthorizePayload in;
    in.order_id = "ord-400";
    in.customer_id = "cust-9";
    in.amount_cents = 9900;
    in.scenario = ScenarioKind::PaymentDecline;

    auto encoded = encode_payment_authorize(in);
    PaymentAuthorizePayload out;
    ASSERT_TRUE(decode_payment_authorize(encoded, out));
    EXPECT_EQ(out.order_id, "ord-400");
    EXPECT_EQ(out.customer_id, "cust-9");
    EXPECT_EQ(out.amount_cents, 9900);
    EXPECT_EQ(out.scenario, ScenarioKind::PaymentDecline);
}

TEST(OrderPlatformMessagesTest, MalformedDecodeRejected) {
    hpactor::StreamBuffer truncated{0x00, 0x00, 0x00, 0x04, 'o'};
    SubmitOrderPayload out;
    EXPECT_FALSE(decode_submit_order(truncated, out));
}

TEST(OrderPlatformMessagesTest, QueryOrderRoundTrip) {
    QueryOrderPayload in;
    in.order_id = "ord-500";

    auto encoded = encode_query_order(in);
    QueryOrderPayload out;
    ASSERT_TRUE(decode_query_order(encoded, out));
    EXPECT_EQ(out.order_id, "ord-500");

    // Malformed rejected.
    hpactor::StreamBuffer truncated{0x00, 0x00, 0x00, 0x02, 'x'};
    EXPECT_FALSE(decode_query_order(truncated, out));
}

TEST(OrderPlatformMessagesTest, ScenarioFromString) {
    EXPECT_EQ(scenario_from_string("happy-path"), ScenarioKind::HappyPath);
    EXPECT_EQ(scenario_from_string("insufficient-stock"),
              ScenarioKind::InsufficientStock);
    EXPECT_EQ(scenario_from_string("payment-decline"), ScenarioKind::PaymentDecline);
    EXPECT_EQ(scenario_from_string("payment-timeout"), ScenarioKind::PaymentTimeout);
    EXPECT_EQ(scenario_from_string("worker-crash"), ScenarioKind::WorkerCrash);
    EXPECT_EQ(scenario_from_string("overload"), ScenarioKind::Overload);
    EXPECT_EQ(scenario_from_string("missing-route"), ScenarioKind::MissingRoute);
    EXPECT_EQ(scenario_from_string("unknown"), ScenarioKind::HappyPath);
}
