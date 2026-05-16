// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <examples/order_platform/messages.hpp>

#include <cassert>
#include <string>
#include <vector>

using namespace hpactor::examples::order_platform;

static void test_submit_order_round_trip() {
    SubmitOrderPayload in;
    in.order_id = "ord-100";
    in.customer_id = "cust-7";
    in.scenario = ScenarioKind::HappyPath;
    in.lines.push_back(OrderLine{"sku-book", 2, 1599});
    in.lines.push_back(OrderLine{"sku-pen", 3, 250});

    auto encoded = encode_submit_order(in);
    SubmitOrderPayload out;
    assert(decode_submit_order(encoded, out));
    assert(out.order_id == "ord-100");
    assert(out.customer_id == "cust-7");
    assert(out.scenario == ScenarioKind::HappyPath);
    assert(out.lines.size() == 2);
    assert(out.lines[0].sku == "sku-book");
    assert(out.lines[0].quantity == 2);
    assert(out.lines[0].unit_cents == 1599);
    assert(out.lines[1].sku == "sku-pen");
    assert(out.lines[1].quantity == 3);
    assert(out.lines[1].unit_cents == 250);
}

static void test_status_round_trip() {
    OrderStatusPayload in;
    in.order_id = "ord-200";
    in.status = OrderStatus::PaymentFailed;
    in.detail = "card declined";
    in.total_cents = 4242;

    auto encoded = encode_order_status(in);
    OrderStatusPayload out;
    assert(decode_order_status(encoded, out));
    assert(out.order_id == "ord-200");
    assert(out.status == OrderStatus::PaymentFailed);
    assert(out.detail == "card declined");
    assert(out.total_cents == 4242);
    assert(to_string(out.status) == std::string("payment_failed"));
}
static void test_inventory_round_trip() {
    InventoryReservePayload in;
    in.order_id = "ord-300";
    in.lines.push_back(OrderLine{"sku-lamp", 1, 3200});

    auto encoded = encode_inventory_reserve(in);
    InventoryReservePayload out;
    assert(decode_inventory_reserve(encoded, out));
    assert(out.order_id == "ord-300");
    assert(out.lines.size() == 1);
    assert(out.lines[0].sku == "sku-lamp");
}

static void test_payment_round_trip() {
    PaymentAuthorizePayload in;
    in.order_id = "ord-400";
    in.customer_id = "cust-9";
    in.amount_cents = 9900;
    in.scenario = ScenarioKind::PaymentDecline;

    auto encoded = encode_payment_authorize(in);
    PaymentAuthorizePayload out;
    assert(decode_payment_authorize(encoded, out));
    assert(out.order_id == "ord-400");
    assert(out.customer_id == "cust-9");
    assert(out.amount_cents == 9900);
    assert(out.scenario == ScenarioKind::PaymentDecline);
}

static void test_malformed_decode_rejected() {
    hpactor::StreamBuffer truncated{0x00, 0x00, 0x00, 0x04, 'o'};
    SubmitOrderPayload out;
    assert(!decode_submit_order(truncated, out));
}

static void test_scenario_from_string() {
    assert(scenario_from_string("happy-path") == ScenarioKind::HappyPath);
    assert(scenario_from_string("insufficient-stock") ==
           ScenarioKind::InsufficientStock);
    assert(scenario_from_string("payment-decline") == ScenarioKind::PaymentDecline);
    assert(scenario_from_string("payment-timeout") == ScenarioKind::PaymentTimeout);
    assert(scenario_from_string("worker-crash") == ScenarioKind::WorkerCrash);
    assert(scenario_from_string("overload") == ScenarioKind::Overload);
    assert(scenario_from_string("missing-route") == ScenarioKind::MissingRoute);
    assert(scenario_from_string("unknown") == ScenarioKind::HappyPath);
}

int main() {
    test_submit_order_round_trip();
    test_status_round_trip();
    test_inventory_round_trip();
    test_payment_round_trip();
    test_malformed_decode_rejected();
    test_scenario_from_string();
    return 0;
}
