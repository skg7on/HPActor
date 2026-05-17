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

// =============================================================================
// HPActor Example 13: Multi-Role Order Platform
// =============================================================================
//
// A five-actor order-processing pipeline demonstrating the full HPActor
// feature surface in a single, self-contained example:
//
//   Actors and workflow:
//     OrderCoordinator  — orchestrates the lifecycle: submit → price →
//                          reserve → authorize → fulfill → complete
//     InventoryActor    — reserves stock or rejects with "insufficient stock"
//     PaymentActor      — authorizes, declines, or simulates a timeout
//     FulfillmentWorker — queues fulfillment or simulates a worker crash
//     OrderLogActor     — append-only event log (circular, capacity 64)
//
//   Features demonstrated:
//     - StatefulActor<T> (coordinator, inventory, log) and EventBasedActor
//       (payment, fulfillment)
//     - TypedMessage with custom TypeTags and hand-rolled binary
//       serialization via BufferWriter / BufferReader
//     - Message routing: context()->send(addr, msg) for directed sends,
//       context()->reply(msg) for request-response reply
//     - Timer scheduling: context()->schedule(delay, msg) for the
//       payment-timeout scenario
//     - Bounded mailboxes: capacity shrinks to 2 in overload mode,
//       forcing overflow into the dead-letter queue
//     - Dead-letter queue: enabled (capacity 128), snapshot inspected
//       after each scenario run
//     - EnqueueResult introspection: try_deliver_local() returns
//       code, depth, capacity, pressure_ratio, and retryable status
//     - Tracing: JSON-file exporter writes span data to
//       build/order-platform-traces.jsonl
//     - HPACTOR_REGISTER_ACTOR for all four worker actor types
//     - std::promise / std::future bridge for collecting the final
//       OrderStatus from outside the actor system
//     - Supervision readiness: FulfillmentWorker calls
//       set_exit_reason(1) in the worker-crash scenario so a
//       supervisor can detect the failure and restart
//
//   Modes (--all-in-one is the primary demo path; other modes are
//   stubs for a distributed deployment):
//
//     --all-in-one       single-process, all actors local, runs one
//                        scenario and prints the result + DLQ snapshot
//     --gateway          long-running gateway role (stub)
//     --inventory        long-running inventory role (stub)
//     --payment          long-running payment role (stub)
//     --fulfillment      long-running fulfillment role (stub)
//     --ops              long-running ops role (stub)
//     --query            query mode (stub, requires --submit demo-order)
//
//   Scenarios (--scenario <name>):
//
//     happy-path         full pipeline succeeds; order completes
//     insufficient-stock inventory rejects; order ends InventoryFailed
//     payment-decline    payment actor declines; order ends PaymentFailed
//     payment-timeout    payment actor drops the message; a 200 ms timer
//                        fires PaymentTimedOut on the coordinator
//     worker-crash       fulfillment worker sets exit_reason(1);
//                        order ends FulfillmentFailed
//     overload           bounded mailbox capacity = 2; 8 rapid sends
//                        overflow into the dead-letter queue
//     missing-route      send to a non-existent ActorId; message lands
//                        in the dead-letter queue
//
//   Quickstart (all-in-one, local actors only):
//
//     ./13_order_platform --all-in-one --scenario happy-path
//     ./13_order_platform --all-in-one --scenario insufficient-stock
//     ./13_order_platform --all-in-one --scenario payment-decline
//     ./13_order_platform --all-in-one --scenario payment-timeout
//     ./13_order_platform --all-in-one --scenario worker-crash
//     ./13_order_platform --all-in-one --scenario overload
//     ./13_order_platform --all-in-one --scenario missing-route
//
//   Distributed (conceptual — stub roles run until Ctrl-C):
//
//     # Terminal 1: payment service
//     ./13_order_platform --payment --actor-port 17132
//
//     # Terminal 2: gateway + remaining workers
//     ./13_order_platform --gateway --actor-port 17130 --http-port 18130
//     --payment 127.0.0.1:17132
//
//     # Terminal 3: query the result
//     ./13_order_platform --query --gateway-port 18130 --submit demo-order
//
//   Optional flags:
//
//     --host 127.0.0.1           bind address for actor and registrar
//     --registrar-host 127.0.0.1 registrar server address
//     --actor-port 17130         actor TCP port
//     --http-port 18130          HTTP gateway port
//     --registrar-port 19153     registrar TCP/UDP port
//     --gateway-port 18130       port for --query to connect to
//
//   Default registrar port is 19153 (not 5353). Port 5353 is commonly
//   used by mDNS (Bonjour) on macOS and Linux, preventing the UDP
//   resolver socket from binding.
//
//   Expected output (happy-path):
//     SCENARIO RESULT order_id=demo-1 status=completed detail=completed
//     total_cents=<n>
//     DLQ depth=0 total_pushed=0 total_lost=0
//
//   Expected output (overload):
//     OVERLOAD enqueue code=... depth=... capacity=2 pressure=...
//     ... (8 lines)
//     OVERLOAD dlq_depth=<n> total_pushed=<n>
//
// =============================================================================

#include <examples/order_platform/messages.hpp>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/actor/typed_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/spawn.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace order = hpactor::examples::order_platform;

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON helpers (order platform schema only)
// ---------------------------------------------------------------------------

[[maybe_unused]] static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"') {
            out += '\\';
            out += '"';
        } else if (c == '\\') {
            out += '\\';
            out += '\\';
        } else {
            out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// State structs
// ---------------------------------------------------------------------------

struct OrderRecord {
    std::string order_id;
    std::string customer_id;
    order::ScenarioKind scenario = order::ScenarioKind::HappyPath;
    order::OrderStatus status = order::OrderStatus::Received;
    std::vector<order::OrderLine> lines;
    uint64_t subtotal_cents = 0;
    uint64_t discount_cents = 0;
    uint64_t tax_cents = 0;
    uint64_t total_cents = 0;
    uint64_t reservation_id = 0;
    std::string detail;
};

struct OrderCoordinatorState {
    std::unordered_map<std::string, OrderRecord> orders;
    uint64_t processed = 0;
};
struct InventoryState {
    std::unordered_map<std::string, uint32_t> stock;
    std::unordered_map<uint64_t, order::InventoryReservePayload> reservations;
    uint64_t next_reservation_id = 1;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t released = 0;
};

struct OrderLogState {
    static constexpr size_t kCapacity = 64;
    std::deque<std::string> entries;
    void append(std::string value) {
        if (entries.size() == kCapacity)
            entries.pop_front();
        entries.push_back(std::move(value));
    }
};

// ---------------------------------------------------------------------------
// Options and CLI
// ---------------------------------------------------------------------------

struct Options {
    std::string mode = "--help";
    order::ScenarioKind scenario = order::ScenarioKind::HappyPath;
    std::string host = "127.0.0.1";
    std::string registrar_host = "127.0.0.1";
    std::string payment_endpoint;
    uint16_t actor_port = 17130;
    uint16_t http_port = 18130;
    uint16_t registrar_port = 19153;
    uint16_t gateway_port = 18130;
    bool submit_demo_order = false;
};

std::atomic<bool> shutdown_requested{false};

void sigint_handler(int) {
    shutdown_requested.store(true, std::memory_order_release);
}

bool parse_port(const std::string& value, uint16_t& port) {
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > 65535)
        return false;
    port = static_cast<uint16_t>(parsed);
    return true;
}

void print_usage(const char* argv0) {
    std::cout
        << "HPActor Example 13: Multi-Role Order Platform\n\n"
        << "Quickstart:\n"
        << "  " << argv0 << " --all-in-one --scenario happy-path\n\n"
        << "Distributed:\n"
        << "  " << argv0 << " --payment --actor-port 17132\n"
        << "  " << argv0 << " --gateway --actor-port 17130 --http-port 18130 "
        << "--payment 127.0.0.1:17132\n"
        << "  " << argv0 << " --query --gateway-port 18130 --submit demo-order\n\n"
        << "Failure scenarios:\n"
        << "  " << argv0 << " --all-in-one --scenario overload\n"
        << "  " << argv0 << " --all-in-one --scenario payment-decline\n";
}

std::optional<Options> parse_args(int argc, char* argv[]) {
    Options opts;
    if (argc <= 1)
        return opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--payment" && opts.mode == "--gateway" && i + 1 < argc &&
            argv[i + 1][0] != '-') {
            opts.payment_endpoint = argv[++i];
        } else if (arg == "--all-in-one" || arg == "--gateway" ||
                   arg == "--inventory" || arg == "--payment" ||
                   arg == "--fulfillment" || arg == "--ops" ||
                   arg == "--query" || arg == "--help") {
            opts.mode = arg;
        } else if (arg == "--scenario") {
            const char* value = need_value("--scenario");
            if (value == nullptr)
                return std::nullopt;
            opts.scenario = order::scenario_from_string(value);
        } else if (arg == "--actor-port") {
            const char* value = need_value("--actor-port");
            if (value == nullptr || !parse_port(value, opts.actor_port))
                return std::nullopt;
        } else if (arg == "--http-port") {
            const char* value = need_value("--http-port");
            if (value == nullptr || !parse_port(value, opts.http_port))
                return std::nullopt;
        } else if (arg == "--gateway-port") {
            const char* value = need_value("--gateway-port");
            if (value == nullptr || !parse_port(value, opts.gateway_port))
                return std::nullopt;
        } else if (arg == "--registrar-port") {
            const char* value = need_value("--registrar-port");
            if (value == nullptr || !parse_port(value, opts.registrar_port))
                return std::nullopt;
        } else if (arg == "--registrar-host") {
            const char* value = need_value("--registrar-host");
            if (value == nullptr)
                return std::nullopt;
            opts.registrar_host = value;
        } else if (arg == "--submit") {
            const char* value = need_value("--submit");
            if (value == nullptr)
                return std::nullopt;
            opts.submit_demo_order = std::string(value) == "demo-order";
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }
    return opts;
}

// ---------------------------------------------------------------------------
// Actors
// ---------------------------------------------------------------------------

class OrderLogActor : public hpactor::StatefulActor<OrderLogState> {
  public:
    static constexpr const char* kActorTypeName = "OrderLogActor";

    OrderLogActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::StatefulActor<OrderLogState>(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == order::LogOrderEventTag) {
                state().append(
                    std::string(msg.payload().begin(), msg.payload().end()));
            }
        }};
    }
};

class InventoryActor : public hpactor::StatefulActor<InventoryState> {
  public:
    static constexpr const char* kActorTypeName = "InventoryActor";

    InventoryActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::StatefulActor<InventoryState>(ctx, sys) {
        state().stock.emplace("sku-book", 100);
        state().stock.emplace("sku-pen", 200);
        state().stock.emplace("sku-lamp", 5);
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == order::ReserveInventoryTag) {
                order::InventoryReservePayload payload;
                if (!order::decode_inventory_reserve(msg.payload(), payload))
                    return;

                bool enough = true;
                for (const auto& line : payload.lines) {
                    auto it = state().stock.find(line.sku);
                    if (it == state().stock.end() || it->second < line.quantity) {
                        enough = false;
                        break;
                    }
                }

                order::InventoryReplyPayload reply;
                reply.order_id = payload.order_id;
                if (enough) {
                    for (const auto& line : payload.lines)
                        state().stock[line.sku] -= line.quantity;
                    reply.ok = true;
                    reply.reservation_id = state().next_reservation_id++;
                    reply.detail = "reserved";
                    state().reservations.emplace(reply.reservation_id, payload);
                    ++state().accepted;
                    context()->reply(hpactor::TypedMessage(
                        order::InventoryReservedTag,
                        order::encode_inventory_reply(reply)));
                } else {
                    reply.ok = false;
                    reply.detail = "insufficient stock";
                    ++state().rejected;
                    context()->reply(hpactor::TypedMessage(
                        order::InventoryRejectedTag,
                        order::encode_inventory_reply(reply)));
                }
            }
        }};
    }
};

class PaymentActor : public hpactor::EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "PaymentActor";

    PaymentActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::AuthorizePaymentTag)
                return;
            order::PaymentAuthorizePayload payload;
            if (!order::decode_payment_authorize(msg.payload(), payload))
                return;

            if (payload.scenario == order::ScenarioKind::PaymentDecline) {
                order::PaymentReplyPayload reply{payload.order_id, false, "",
                                                 "card declined"};
                context()->reply(hpactor::TypedMessage(
                    order::PaymentDeclinedTag, order::encode_payment_reply(reply)));
                return;
            }
            if (payload.scenario == order::ScenarioKind::PaymentTimeout) {
                return;
            }

            order::PaymentReplyPayload reply;
            reply.order_id = payload.order_id;
            reply.ok = true;
            reply.authorization_id = "auth-" + payload.order_id;
            reply.detail = "authorized";
            context()->reply(hpactor::TypedMessage(
                order::PaymentAuthorizedTag, order::encode_payment_reply(reply)));
        }};
    }
};

class FulfillmentWorkerActor : public hpactor::EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "FulfillmentWorkerActor";

    FulfillmentWorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::QueueFulfillmentTag)
                return;
            order::FulfillmentPayload payload;
            if (!order::decode_fulfillment(msg.payload(), payload))
                return;
            if (payload.scenario == order::ScenarioKind::WorkerCrash) {
                context()->reply(
                    hpactor::TypedMessage(order::FulfillmentFailedTag,
                                          order::encode_fulfillment(payload)));
                set_exit_reason(1);
                return;
            }
            context()->reply(hpactor::TypedMessage(
                order::FulfillmentQueuedTag, order::encode_fulfillment(payload)));
        }};
    }
};

class OrderCoordinatorActor : public hpactor::StatefulActor<OrderCoordinatorState> {
  public:
    static constexpr const char* kActorTypeName = "OrderCoordinatorActor";

    OrderCoordinatorActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                          hpactor::ActorAddress inventory,
                          hpactor::ActorAddress payment,
                          hpactor::ActorAddress fulfillment,
                          hpactor::ActorAddress log,
                          std::promise<order::OrderStatusPayload>* done)
        : hpactor::StatefulActor<OrderCoordinatorState>(ctx, sys),
          inventory_(inventory), payment_(payment), fulfillment_(fulfillment),
          log_(log), done_(done) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            ++state().processed;
            if (msg.type_id() == order::SubmitOrderTag) {
                on_submit(msg);
            } else if (msg.type_id() == order::InventoryReservedTag ||
                       msg.type_id() == order::InventoryRejectedTag) {
                on_inventory(msg);
            } else if (msg.type_id() == order::PaymentAuthorizedTag ||
                       msg.type_id() == order::PaymentDeclinedTag) {
                on_payment(msg);
            } else if (msg.type_id() == order::FulfillmentQueuedTag ||
                       msg.type_id() == order::FulfillmentFailedTag) {
                on_fulfillment(msg);
            } else if (msg.type_id() == order::PaymentTimedOutTag) {
                on_payment_timeout(msg);
            } else if (msg.type_id() == order::QueryOrderTag) {
                on_query(msg);
            }
        }};
    }

  private:
    void on_query(hpactor::TypedMessage& msg) {
        order::QueryOrderPayload query;
        if (!order::decode_query_order(msg.payload(), query))
            return;
        auto it = state().orders.find(query.order_id);
        order::OrderStatusPayload status;
        status.order_id = query.order_id;
        if (it == state().orders.end()) {
            status.status = order::OrderStatus::Cancelled;
            status.detail = "not found";
        } else {
            const auto& record = it->second;
            status.status = record.status;
            status.detail = record.detail;
            status.total_cents = record.total_cents;
        }
        context()->reply(hpactor::TypedMessage(
            order::OrderStatusTag, order::encode_order_status(status)));
    }

    void log_event(const std::string& text) {
        context()->send(log_, hpactor::TypedMessage(
                                  order::LogOrderEventTag,
                                  hpactor::StreamBuffer(text.begin(), text.end())));
    }

    void complete(const OrderRecord& record) {
        order::OrderStatusPayload result;
        result.order_id = record.order_id;
        result.status = record.status;
        result.detail = record.detail;
        result.total_cents = record.total_cents;
        if (done_ != nullptr) {
            done_->set_value(result);
            done_ = nullptr;
        }
    }

    void on_submit(hpactor::TypedMessage& msg) {
        order::SubmitOrderPayload payload;
        if (!order::decode_submit_order(msg.payload(), payload))
            return;

        OrderRecord record;
        record.order_id = payload.order_id;
        record.customer_id = payload.customer_id;
        record.scenario = payload.scenario;
        record.lines = payload.lines;
        record.subtotal_cents = order::calculate_subtotal(payload.lines);
        record.discount_cents = record.subtotal_cents >= 5000 ? 500 : 0;
        record.tax_cents = (record.subtotal_cents - record.discount_cents) / 10;
        record.total_cents =
            record.subtotal_cents - record.discount_cents + record.tax_cents;
        record.status = order::OrderStatus::Priced;
        state().orders[record.order_id] = record;
        log_event(record.order_id +
                  " priced total=" + std::to_string(record.total_cents));

        order::InventoryReservePayload reserve{record.order_id, record.lines};
        context()->send(inventory_, hpactor::TypedMessage(
                                        order::ReserveInventoryTag,
                                        order::encode_inventory_reserve(reserve)));
    }

    void on_inventory(hpactor::TypedMessage& msg) {
        order::InventoryReplyPayload reply;
        if (!order::decode_inventory_reply(msg.payload(), reply))
            return;
        auto it = state().orders.find(reply.order_id);
        if (it == state().orders.end())
            return;
        auto& record = it->second;
        if (!reply.ok) {
            record.status = order::OrderStatus::InventoryFailed;
            record.detail = reply.detail;
            log_event(record.order_id + " inventory_failed");
            complete(record);
            return;
        }
        record.status = order::OrderStatus::InventoryReserved;
        record.reservation_id = reply.reservation_id;
        log_event(record.order_id + " inventory_reserved");

        order::PaymentAuthorizePayload payment;
        payment.order_id = record.order_id;
        payment.customer_id = record.customer_id;
        payment.amount_cents = record.total_cents;
        payment.scenario = record.scenario;
        context()->send(payment_, hpactor::TypedMessage(
                                      order::AuthorizePaymentTag,
                                      order::encode_payment_authorize(payment)));

        if (record.scenario == order::ScenarioKind::PaymentTimeout) {
            order::OrderStatusPayload timeout_marker;
            timeout_marker.order_id = record.order_id;
            context()->schedule(
                std::chrono::milliseconds(200),
                hpactor::TypedMessage(order::PaymentTimedOutTag,
                                      order::encode_order_status(timeout_marker)));
        }
    }

    void on_payment(hpactor::TypedMessage& msg) {
        order::PaymentReplyPayload reply;
        if (!order::decode_payment_reply(msg.payload(), reply))
            return;
        auto it = state().orders.find(reply.order_id);
        if (it == state().orders.end())
            return;
        auto& record = it->second;
        if (!reply.ok) {
            record.status = order::OrderStatus::PaymentFailed;
            record.detail = reply.detail;
            log_event(record.order_id + " payment_failed");
            complete(record);
            return;
        }
        record.status = order::OrderStatus::PaymentAuthorized;
        log_event(record.order_id + " payment_authorized");
        order::FulfillmentPayload fulfill;
        fulfill.order_id = record.order_id;
        fulfill.reservation_id = record.reservation_id;
        fulfill.scenario = record.scenario;
        context()->send(fulfillment_,
                        hpactor::TypedMessage(order::QueueFulfillmentTag,
                                              order::encode_fulfillment(fulfill)));
    }

    void on_payment_timeout(hpactor::TypedMessage& msg) {
        order::OrderStatusPayload timeout;
        if (!order::decode_order_status(msg.payload(), timeout))
            return;
        auto it = state().orders.find(timeout.order_id);
        if (it == state().orders.end())
            return;
        auto& record = it->second;
        if (record.status == order::OrderStatus::InventoryReserved) {
            record.status = order::OrderStatus::PaymentTimedOut;
            record.detail = "payment timed out";
            log_event(record.order_id + " payment_timed_out");
            complete(record);
        }
    }

    void on_fulfillment(hpactor::TypedMessage& msg) {
        order::FulfillmentPayload reply;
        if (!order::decode_fulfillment(msg.payload(), reply))
            return;
        auto it = state().orders.find(reply.order_id);
        if (it == state().orders.end())
            return;
        auto& record = it->second;
        if (msg.type_id() == order::FulfillmentFailedTag) {
            record.status = order::OrderStatus::FulfillmentFailed;
            record.detail = "worker failed";
            log_event(record.order_id + " fulfillment_failed");
            complete(record);
            return;
        }
        record.status = order::OrderStatus::Completed;
        record.detail = "completed";
        log_event(record.order_id + " completed");
        complete(record);
    }

    hpactor::ActorAddress inventory_;
    hpactor::ActorAddress payment_;
    hpactor::ActorAddress fulfillment_;
    hpactor::ActorAddress log_;
    std::promise<order::OrderStatusPayload>* done_ = nullptr;
};

HPACTOR_REGISTER_ACTOR("OrderLogActor", OrderLogActor)
HPACTOR_REGISTER_ACTOR("InventoryActor", InventoryActor)
HPACTOR_REGISTER_ACTOR("PaymentActor", PaymentActor)
HPACTOR_REGISTER_ACTOR("FulfillmentWorkerActor", FulfillmentWorkerActor)

// ---------------------------------------------------------------------------
// Ops probe actor
// ---------------------------------------------------------------------------

class OpsProbeActor : public hpactor::EventBasedActor {
  public:
    OpsProbeActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                  std::chrono::milliseconds interval = std::chrono::seconds(1))
        : hpactor::EventBasedActor(ctx, sys), interval_(interval) {
        become(make_behavior());
        // Kick off the first tick.
        context()->schedule(interval_,
                            hpactor::TypedMessage(order::OpsProbeTickTag,
                                                  hpactor::StreamBuffer{}));
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::OpsProbeTickTag)
                return;
            print_tick();
            // Self-reschedule.
            context()->schedule(interval_,
                                hpactor::TypedMessage(order::OpsProbeTickTag,
                                                      hpactor::StreamBuffer{}));
        }};
    }

  private:
    void print_tick() {
        auto& sys = system();
        auto* sched = sys.scheduler();
        auto dlq = sys.dead_letter_snapshot();

        std::cout << "[" << tick_ << "] "
                  << "actors=" << sys.actor_count() << " "
                  << "dlq_depth=" << dlq.depth << " "
                  << "dlq_pushed=" << dlq.total_pushed << " "
                  << "dlq_lost=" << dlq.total_lost;
        if (sched) {
            std::cout << " workers=" << sched->worker_count();
        }
        std::cout << "\n";
        ++tick_;
    }

    std::chrono::milliseconds interval_;
    uint64_t tick_ = 0;
};

// ---------------------------------------------------------------------------
// Dead-letter record helpers
// ---------------------------------------------------------------------------

const char* dead_letter_reason_name(hpactor::mailbox::DeadLetterReason reason) {
    using R = hpactor::mailbox::DeadLetterReason;
    switch (reason) {
        case R::MailboxFull:
            return "MailboxFull";
        case R::MailboxClosed:
            return "MailboxClosed";
        case R::ActorNotFound:
            return "ActorNotFound";
        case R::ActorTerminated:
            return "ActorTerminated";
        case R::MissingRoute:
            return "MissingRoute";
        case R::RemoteNodeUnreachable:
            return "RemoteNodeUnreachable";
        case R::NetworkPartition:
            return "NetworkPartition";
        case R::TransportSendFailed:
            return "TransportSendFailed";
        case R::DecodeFailed:
            return "DecodeFailed";
        case R::OverflowPolicy:
            return "OverflowPolicy";
        case R::NoDropRejected:
            return "NoDropRejected";
        case R::DrainTimeout:
            return "DrainTimeout";
        case R::DrainPolicyDrop:
            return "DrainPolicyDrop";
        default:
            return "Unknown";
    }
}

void print_dead_letter_records(hpactor::ActorSystem& system) {
    hpactor::mailbox::DeadLetterRecord record;
    size_t count = 0;
    while (system.pop_dead_letter(record)) {
        std::cout << "  DLQ[" << count << "] "
                  << "reason=" << dead_letter_reason_name(record.reason)
                  << " type_tag=0x" << std::hex
                  << static_cast<uint32_t>(record.type_tag) << std::dec
                  << " sender_id=" << record.sender.id.value()
                  << " target_id=" << record.target.id.value()
                  << " depth=" << record.mailbox_depth << "/"
                  << record.mailbox_capacity
                  << " payload_bytes=" << record.payload_size << "\n";
        if (!record.payload_sample.empty()) {
            size_t sample_len = std::min(record.payload_sample.size(), size_t(64));
            std::string sample(record.payload_sample.begin(),
                               record.payload_sample.begin() + sample_len);
            std::cout << "    sample: " << sample << "\n";
        }
        ++count;
    }
    if (count == 0) {
        std::cout << "  (no dead-letter records)\n";
    } else {
        std::cout << "  total: " << count << " records\n";
    }
}

// ---------------------------------------------------------------------------
// Config and runners
// ---------------------------------------------------------------------------

const char* enqueue_code_name(hpactor::mailbox::EnqueueResultCode code) {
    using Code = hpactor::mailbox::EnqueueResultCode;
    switch (code) {
        case Code::Accepted:
            return "accepted";
        case Code::AcceptedWithSoftPressure:
            return "accepted_with_soft_pressure";
        case Code::Rejected:
            return "rejected";
        case Code::DroppedNewest:
            return "dropped_newest";
        case Code::DroppedExisting:
            return "dropped_existing";
        case Code::ReroutedToDeadLetter:
            return "rerouted_to_dead_letter";
        case Code::ReroutedToOverflow:
            return "rerouted_to_overflow";
        case Code::MailboxClosed:
            return "mailbox_closed";
        case Code::ActorNotFound:
            return "actor_not_found";
    }
    return "rejected";
}

void print_enqueue_result(const char* label,
                          const hpactor::mailbox::EnqueueResult& result) {
    std::cout << label << " code=" << enqueue_code_name(result.code)
              << " depth=" << result.depth << " capacity=" << result.capacity
              << " pressure=" << result.pressure_ratio
              << " retryable=" << (result.retryable() ? "true" : "false") << "\n";
}

hpactor::Config make_base_config(const Options& opts, uint16_t actor_port) {
    hpactor::Config config;
    config.scheduler_threads = 4;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint(
        opts.host + ":" + std::to_string(actor_port));
    config.tcp_port = actor_port;
    config.enable_network = actor_port != 0;
    config.registrar.tcp_port = opts.registrar_port;
    config.registrar.udp_port = opts.registrar_port;
    config.mailbox.default_capacity =
        opts.scenario == order::ScenarioKind::Overload ? 2 : 1024;
    config.dead_letters.enabled = true;
    config.dead_letters.capacity = 128;
    config.tracing.enabled = true;
    config.tracing.exporter = hpactor::tracing::TraceExporterKind::kJsonFile;
    config.tracing.json_file_path = "build/order-platform-traces.jsonl";
    config.cli.enabled = false;
    return config;
}

void run_until_signal(const char* role) {
    std::signal(SIGINT, sigint_handler);
    std::cout << role << " running. Press Ctrl-C to stop.\n";
    while (!shutdown_requested.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int run_all_in_one(const Options& opts) {
    hpactor::Config config = make_base_config(opts, 0);
    config.enable_network = false;
    hpactor::ActorSystem system(config);

    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto payment = system.spawn<PaymentActor>();
    auto fulfillment_worker = system.spawn<FulfillmentWorkerActor>();

    std::promise<order::OrderStatusPayload> done;
    auto result_future = done.get_future();
    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(), payment.address(), fulfillment_worker.address(),
        log.address(), &done);

    order::SubmitOrderPayload submit;
    submit.order_id = "demo-1";
    submit.customer_id = "customer-1";
    submit.scenario = opts.scenario;
    submit.lines.push_back(order::OrderLine{"sku-book", 2, 1599});
    submit.lines.push_back(order::OrderLine{"sku-pen", 3, 250});
    if (opts.scenario == order::ScenarioKind::InsufficientStock) {
        submit.lines.push_back(order::OrderLine{"sku-lamp", 99, 3200});
    }

    if (opts.scenario == order::ScenarioKind::Overload) {
        for (int i = 0; i < 8; ++i) {
            order::SubmitOrderPayload burst = submit;
            burst.order_id = "burst-" + std::to_string(i);
            auto result = system.try_deliver_local(
                coordinator.id(),
                hpactor::TypedMessage(order::SubmitOrderTag,
                                      order::encode_submit_order(burst)));
            print_enqueue_result("OVERLOAD enqueue", result);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto dlq = system.dead_letter_snapshot();
        std::cout << "OVERLOAD dlq_depth=" << dlq.depth
                  << " total_pushed=" << dlq.total_pushed << "\n";
        std::cout << "--- Dead-Letter Records ---\n";
        print_dead_letter_records(system);
        return 0;
    }

    if (opts.scenario == order::ScenarioKind::MissingRoute) {
        auto result = system.try_deliver_local(
            hpactor::ActorId{999999},
            hpactor::TypedMessage(order::SubmitOrderTag,
                                  order::encode_submit_order(submit)));
        print_enqueue_result("MISSING_ROUTE enqueue", result);
        auto dlq = system.dead_letter_snapshot();
        std::cout << "MISSING_ROUTE dlq_depth=" << dlq.depth
                  << " total_pushed=" << dlq.total_pushed << "\n";
        std::cout << "--- Dead-Letter Records ---\n";
        print_dead_letter_records(system);
        return 0;
    }

    system.deliver_local(coordinator.id(),
                         hpactor::TypedMessage(order::SubmitOrderTag,
                                               order::encode_submit_order(submit)));

    auto status = result_future.wait_for(std::chrono::seconds(3));
    if (status == std::future_status::timeout) {
        std::cout << "SCENARIO RESULT order_id=demo-1 status=timeout\n";
        return 2;
    }
    auto final_status = result_future.get();
    std::cout << "SCENARIO RESULT order_id=" << final_status.order_id
              << " status=" << order::to_string(final_status.status)
              << " detail=" << final_status.detail
              << " total_cents=" << final_status.total_cents << "\n";

    auto dlq = system.dead_letter_snapshot();
    std::cout << "DLQ depth=" << dlq.depth << " total_pushed=" << dlq.total_pushed
              << " total_lost=" << dlq.total_lost << "\n";

    std::cout << "--- Dead-Letter Records ---\n";
    print_dead_letter_records(system);
    return 0;
}

int run_long_role(const Options& opts, const char* role) {
    hpactor::ActorSystem system(make_base_config(opts, opts.actor_port));
    std::cout << role << " endpoint="
              << hpactor::endpoint_ops::to_string(system.endpoint()) << "\n";
    run_until_signal(role);
    return 0;
}

int run_ops(const Options& opts) {
    hpactor::Config config = make_base_config(opts, opts.actor_port);
    config.cli.enabled = true;
    hpactor::ActorSystem system(config);

    // Spawn the probe actor for periodic health output.
    system.spawn<OpsProbeActor>(std::chrono::seconds(1));

    std::cout << "OPS probe running (interval=1s, actors=" << system.actor_count()
              << ")\n";

    run_until_signal("OPS");

    // Final DLQ dump on exit.
    std::cout << "--- Final Dead-Letter Records ---\n";
    print_dead_letter_records(system);
    return 0;
}

int run_payment(const Options& opts) {
    hpactor::Config config = make_base_config(opts, opts.actor_port);
    config.enable_network = true;
    config.tcp_port = opts.actor_port;
    hpactor::ActorSystem system(config);

    // Register PaymentActor so remote spawn requests can create instances.
    system.actor_type_registry().register_type<PaymentActor>("PaymentActor");

    std::cout << "PAYMENT node listening on "
              << hpactor::endpoint_ops::to_string(system.endpoint())
              << " (tcp=" << opts.actor_port << ")\n";
    run_until_signal("PAYMENT");
    return 0;
}

int run_gateway(const Options& opts) {
    hpactor::Config config = make_base_config(opts, opts.actor_port);
    config.enable_network = true;
    config.tcp_port = opts.actor_port;

    // Add static route for remote payment endpoint before ActorSystem
    // construction, so the transport can resolve it.
    if (!opts.payment_endpoint.empty()) {
        auto remote_ep =
            hpactor::endpoint_ops::parse_endpoint(opts.payment_endpoint);
        // Extract host string from the endpoint.
        std::string payment_host =
            opts.payment_endpoint.substr(0, opts.payment_endpoint.find(':'));
        config.registrar.static_routes.push_back(hpactor::net::StaticRouteConfig{
            remote_ep, payment_host,
            static_cast<uint16_t>(std::stoi(opts.payment_endpoint.substr(
                opts.payment_endpoint.find(':') + 1)))});
    }

    hpactor::ActorSystem system(config);

    // Spawn local actors.
    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto fulfillment_worker = system.spawn<FulfillmentWorkerActor>();

    // Resolve payment actor — local or remote.
    hpactor::ActorAddress payment_addr;
    if (!opts.payment_endpoint.empty()) {
        // Remote-spawn PaymentActor on the payment node.
        hpactor::StreamBuffer args;
        hpactor::AsyncActor async =
            system.spawn_remote_async(opts.payment_endpoint, "PaymentActor", args);

        std::cout << "Remote-spawning PaymentActor on " << opts.payment_endpoint
                  << "...\n";

        auto async_result = async.get();
        if (!async_result.has_value()) {
            std::cerr << "Failed to remote-spawn PaymentActor: "
                      << async_result.error().message() << "\n";
            return 1;
        }
        payment_addr = async_result.value().address();
        std::cout << "Remote PaymentActor spawned: id=" << payment_addr.id.value()
                  << "\n";
    } else {
        auto payment = system.spawn<PaymentActor>();
        payment_addr = payment.address();
        std::cout << "Local PaymentActor spawned\n";
    }

    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(), payment_addr, fulfillment_worker.address(),
        log.address(),
        nullptr); // long-running, no completion promise

    // Spawn HTTP gateway on the configured port.
    auto gw_handle =
        system.spawn<hpactor::net::HTTPGatewayActor>(opts.host, opts.http_port);
    auto gw =
        std::static_pointer_cast<hpactor::net::HTTPGatewayActor>(gw_handle.get());

    auto coordinator_addr = coordinator.address();

    // POST /orders — submit a new order via JSON body.
    gw->route(
        hpactor::net::HttpMethod::POST, "/orders",
        [coordinator_addr](const hpactor::net::HttpRequest& req)
            -> std::pair<hpactor::ActorAddress, hpactor::TypedMessage> {
            std::string body(req.body.begin(), req.body.end());
            order::SubmitOrderPayload submit;
            // Find the order_id field: "order_id":"..."
            auto pos = body.find("\"order_id\"");
            if (pos == std::string::npos) {
                return {coordinator_addr,
                        hpactor::TypedMessage(order::SubmitOrderTag,
                                              hpactor::StreamBuffer{})};
            }
            // Skip "order_id":" to find the value.
            pos = body.find('"', pos + 11);
            if (pos == std::string::npos)
                pos = body.find("order_id") + 9;
            else
                pos = pos + 1;
            auto end = body.find('"', pos);
            if (end != std::string::npos)
                submit.order_id = body.substr(pos, end - pos);

            // Extract customer_id.
            pos = body.find("\"customer_id\"");
            if (pos != std::string::npos) {
                pos = body.find('"', pos + 14);
                if (pos != std::string::npos) {
                    ++pos;
                    end = body.find('"', pos);
                    if (end != std::string::npos)
                        submit.customer_id = body.substr(pos, end - pos);
                }
            }

            // Extract scenario if present.
            pos = body.find("\"scenario\"");
            if (pos != std::string::npos) {
                pos = body.find('"', pos + 10);
                if (pos != std::string::npos) {
                    ++pos;
                    end = body.find('"', pos);
                    if (end != std::string::npos)
                        submit.scenario = order::scenario_from_string(
                            body.substr(pos, end - pos));
                }
            }

            // Extract lines array.
            pos = body.find("\"lines\"");
            if (pos != std::string::npos) {
                // Parse each object in the array:
                // {"sku":"...","quantity":N,"unit_cents":M}
                size_t obj_start = body.find('{', pos);
                while (obj_start != std::string::npos &&
                       obj_start < body.find(']', pos)) {
                    order::OrderLine line;
                    auto sku_pos = body.find("\"sku\"", obj_start);
                    if (sku_pos != std::string::npos) {
                        sku_pos = body.find('"', sku_pos + 6);
                        if (sku_pos != std::string::npos) {
                            ++sku_pos;
                            auto sku_end = body.find('"', sku_pos);
                            if (sku_end != std::string::npos)
                                line.sku = body.substr(sku_pos, sku_end - sku_pos);
                        }
                    }
                    auto qty_pos = body.find("\"quantity\"", obj_start);
                    if (qty_pos != std::string::npos) {
                        qty_pos = body.find(':', qty_pos) + 1;
                        line.quantity = static_cast<uint32_t>(
                            std::strtoul(body.c_str() + qty_pos, nullptr, 10));
                    }
                    auto uc_pos = body.find("\"unit_cents\"", obj_start);
                    if (uc_pos != std::string::npos) {
                        uc_pos = body.find(':', uc_pos) + 1;
                        line.unit_cents =
                            std::strtoull(body.c_str() + uc_pos, nullptr, 10);
                    }
                    submit.lines.push_back(std::move(line));
                    obj_start = body.find('{', obj_start + 1);
                }
            }

            return {coordinator_addr,
                    hpactor::TypedMessage(order::SubmitOrderTag,
                                          order::encode_submit_order(submit))};
        });

    // GET /orders/:id — query order status.
    gw->route(hpactor::net::HttpMethod::GET, "/orders/:id",
              [coordinator_addr](const hpactor::net::HttpRequest& req)
                  -> std::pair<hpactor::ActorAddress, hpactor::TypedMessage> {
                  auto it = req.path_params.find("id");
                  std::string order_id =
                      (it != req.path_params.end()) ? it->second : "";
                  order::QueryOrderPayload query{order_id};
                  return {coordinator_addr,
                          hpactor::TypedMessage(order::QueryOrderTag,
                                                order::encode_query_order(query))};
              });

    std::cout << "GATEWAY listening on http://" << opts.host << ":"
              << opts.http_port << "\n";
    run_until_signal("GATEWAY");
    return 0;
}

int run_query(const Options& opts) {
    if (!opts.submit_demo_order) {
        std::cout << "QUERY requires --submit demo-order\n";
        return 1;
    }

    // Build a demo order as JSON.
    std::ostringstream json;
    json << R"({"order_id":"demo-1")"
         << R"(,"customer_id":"customer-1")"
         << R"(,"scenario":")" << order::to_string(opts.scenario) << R"(")"
         << R"(,"lines":[{"sku":"sku-book","quantity":2,"unit_cents":1599}])"
         << "}";
    std::string json_body = json.str();
    hpactor::StreamBuffer body(json_body.begin(), json_body.end());

    // Use HttpClient to POST the order.
    hpactor::net::EventLoop loop;
    hpactor::net::HttpClient client(&loop);

    std::string url = "http://" + opts.host + ":" +
                      std::to_string(opts.gateway_port) + "/orders";

    auto future = client.post(url, body, {{"Content-Type", "application/json"}});

    auto result = future.get();
    if (!result.has_value()) {
        std::cerr << "QUERY failed: " << result.error().message() << "\n";
        return 1;
    }

    auto& response = result.value();
    std::string response_str(response.begin(), response.end());
    std::cout << "QUERY response: " << response_str << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);
    if (!opts.has_value() || opts->mode == "--help") {
        print_usage(argv[0]);
        return opts.has_value() ? 0 : 1;
    }

    if (opts->mode == "--all-in-one")
        return run_all_in_one(*opts);
    if (opts->mode == "--query")
        return run_query(*opts);
    if (opts->mode == "--gateway")
        return run_gateway(*opts);
    if (opts->mode == "--inventory")
        return run_long_role(*opts, "INVENTORY");
    if (opts->mode == "--payment")
        return run_payment(*opts);
    if (opts->mode == "--fulfillment")
        return run_long_role(*opts, "FULFILLMENT");
    if (opts->mode == "--ops")
        return run_ops(*opts);

    print_usage(argv[0]);
    return 1;
}
