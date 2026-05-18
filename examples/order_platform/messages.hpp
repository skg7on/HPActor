// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor::examples::order_platform {

inline constexpr TypeTag SubmitOrderTag{0x00020000};
inline constexpr TypeTag OrderAcceptedTag{0x00020001};
inline constexpr TypeTag QueryOrderTag{0x00020002};
inline constexpr TypeTag OrderStatusTag{0x00020003};
inline constexpr TypeTag ReserveInventoryTag{0x00020004};
inline constexpr TypeTag InventoryReservedTag{0x00020005};
inline constexpr TypeTag InventoryRejectedTag{0x00020006};
inline constexpr TypeTag ReleaseInventoryTag{0x00020007};
inline constexpr TypeTag AuthorizePaymentTag{0x00020008};
inline constexpr TypeTag PaymentAuthorizedTag{0x00020009};
inline constexpr TypeTag PaymentDeclinedTag{0x0002000A};
inline constexpr TypeTag PaymentTimedOutTag{0x0002000B};
inline constexpr TypeTag QueueFulfillmentTag{0x0002000C};
inline constexpr TypeTag FulfillmentQueuedTag{0x0002000D};
inline constexpr TypeTag FulfillmentFailedTag{0x0002000E};
inline constexpr TypeTag LogOrderEventTag{0x0002000F};
inline constexpr TypeTag ScenarioKickTag{0x00020010};
inline constexpr TypeTag OpsProbeTickTag{0x00020011};
inline constexpr TypeTag PricingRequestTag{0x00020012};
inline constexpr TypeTag PricingReplyTag{0x00020013};

enum class ScenarioKind : uint8_t {
    HappyPath = 0,
    InsufficientStock,
    PaymentDecline,
    PaymentTimeout,
    WorkerCrash,
    Overload,
    MissingRoute,
};

enum class OrderStatus : uint8_t {
    Received = 0,
    Priced,
    InventoryReserved,
    PaymentAuthorized,
    FulfillmentQueued,
    Completed,
    Rejected,
    Cancelled,
    InventoryFailed,
    PaymentFailed,
    PaymentTimedOut,
    FulfillmentFailed,
    Overloaded,
};

struct OrderLine {
    std::string sku;
    uint32_t quantity = 0;
    uint64_t unit_cents = 0;
};

struct SubmitOrderPayload {
    std::string order_id;
    std::string customer_id;
    std::vector<OrderLine> lines;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct OrderStatusPayload {
    std::string order_id;
    OrderStatus status = OrderStatus::Received;
    std::string detail;
    uint64_t total_cents = 0;
};

struct InventoryReservePayload {
    std::string order_id;
    std::vector<OrderLine> lines;
};

struct InventoryReplyPayload {
    std::string order_id;
    bool ok = false;
    uint64_t reservation_id = 0;
    std::string detail;
};

struct PaymentAuthorizePayload {
    std::string order_id;
    std::string customer_id;
    uint64_t amount_cents = 0;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct PaymentReplyPayload {
    std::string order_id;
    bool ok = false;
    std::string authorization_id;
    std::string detail;
};

struct FulfillmentPayload {
    std::string order_id;
    uint64_t reservation_id = 0;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct QueryOrderPayload {
    std::string order_id;
};

struct PricingRequest {
    std::string order_id;
    std::vector<OrderLine> lines;
};

struct PricingReply {
    std::string order_id;
    uint64_t subtotal_cents = 0;
    uint64_t discount_cents = 0;
    uint64_t tax_cents = 0;
    uint64_t total_cents = 0;
};

class BufferWriter {
  public:
    void u8(uint8_t value) {
        buffer_.push_back(value);
    }

    void u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void str(const std::string& value) {
        u32(static_cast<uint32_t>(value.size()));
        auto begin = reinterpret_cast<const uint8_t*>(value.data());
        buffer_.insert(buffer_.end(), begin, begin + value.size());
    }

    StreamBuffer finish() {
        return std::move(buffer_);
    }

  private:
    StreamBuffer buffer_;
};

class BufferReader {
  public:
    explicit BufferReader(const StreamBuffer& buffer) : buffer_(buffer) {}

    bool u8(uint8_t& value) {
        if (offset_ + 1 > buffer_.size())
            return false;
        value = buffer_[offset_++];
        return true;
    }

    bool u32(uint32_t& value) {
        if (offset_ + 4 > buffer_.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
            value = (value << 8) | buffer_[offset_++];
        return true;
    }

    bool u64(uint64_t& value) {
        if (offset_ + 8 > buffer_.size())
            return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value = (value << 8) | buffer_[offset_++];
        return true;
    }

    bool str(std::string& value) {
        uint32_t size = 0;
        if (!u32(size))
            return false;
        if (offset_ + size > buffer_.size())
            return false;
        value.assign(reinterpret_cast<const char*>(buffer_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    bool done() const {
        return offset_ == buffer_.size();
    }

  private:
    const StreamBuffer& buffer_;
    size_t offset_ = 0;
};

inline const char* to_string(ScenarioKind value) {
    switch (value) {
        case ScenarioKind::HappyPath:
            return "happy-path";
        case ScenarioKind::InsufficientStock:
            return "insufficient-stock";
        case ScenarioKind::PaymentDecline:
            return "payment-decline";
        case ScenarioKind::PaymentTimeout:
            return "payment-timeout";
        case ScenarioKind::WorkerCrash:
            return "worker-crash";
        case ScenarioKind::Overload:
            return "overload";
        case ScenarioKind::MissingRoute:
            return "missing-route";
    }
    return "happy-path";
}

inline ScenarioKind scenario_from_string(std::string_view value) {
    if (value == "insufficient-stock")
        return ScenarioKind::InsufficientStock;
    if (value == "payment-decline")
        return ScenarioKind::PaymentDecline;
    if (value == "payment-timeout")
        return ScenarioKind::PaymentTimeout;
    if (value == "worker-crash")
        return ScenarioKind::WorkerCrash;
    if (value == "overload")
        return ScenarioKind::Overload;
    if (value == "missing-route")
        return ScenarioKind::MissingRoute;
    return ScenarioKind::HappyPath;
}

inline const char* to_string(OrderStatus value) {
    switch (value) {
        case OrderStatus::Received:
            return "received";
        case OrderStatus::Priced:
            return "priced";
        case OrderStatus::InventoryReserved:
            return "inventory_reserved";
        case OrderStatus::PaymentAuthorized:
            return "payment_authorized";
        case OrderStatus::FulfillmentQueued:
            return "fulfillment_queued";
        case OrderStatus::Completed:
            return "completed";
        case OrderStatus::Rejected:
            return "rejected";
        case OrderStatus::Cancelled:
            return "cancelled";
        case OrderStatus::InventoryFailed:
            return "inventory_failed";
        case OrderStatus::PaymentFailed:
            return "payment_failed";
        case OrderStatus::PaymentTimedOut:
            return "payment_timed_out";
        case OrderStatus::FulfillmentFailed:
            return "fulfillment_failed";
        case OrderStatus::Overloaded:
            return "overloaded";
    }
    return "received";
}

inline void
encode_lines(BufferWriter& writer, const std::vector<OrderLine>& lines) {
    writer.u32(static_cast<uint32_t>(lines.size()));
    for (const auto& line : lines) {
        writer.str(line.sku);
        writer.u32(line.quantity);
        writer.u64(line.unit_cents);
    }
}

inline bool decode_lines(BufferReader& reader, std::vector<OrderLine>& lines) {
    uint32_t count = 0;
    if (!reader.u32(count))
        return false;
    lines.clear();
    lines.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        OrderLine line;
        if (!reader.str(line.sku))
            return false;
        if (!reader.u32(line.quantity))
            return false;
        if (!reader.u64(line.unit_cents))
            return false;
        lines.push_back(std::move(line));
    }
    return true;
}

inline StreamBuffer encode_submit_order(const SubmitOrderPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.str(payload.customer_id);
    writer.u8(static_cast<uint8_t>(payload.scenario));
    encode_lines(writer, payload.lines);
    return writer.finish();
}

inline bool
decode_submit_order(const StreamBuffer& buffer, SubmitOrderPayload& out) {
    BufferReader reader(buffer);
    uint8_t scenario = 0;
    if (!reader.str(out.order_id))
        return false;
    if (!reader.str(out.customer_id))
        return false;
    if (!reader.u8(scenario))
        return false;
    out.scenario = static_cast<ScenarioKind>(scenario);
    if (!decode_lines(reader, out.lines))
        return false;
    return reader.done();
}

inline StreamBuffer encode_order_status(const OrderStatusPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u8(static_cast<uint8_t>(payload.status));
    writer.str(payload.detail);
    writer.u64(payload.total_cents);
    return writer.finish();
}

inline bool
decode_order_status(const StreamBuffer& buffer, OrderStatusPayload& out) {
    BufferReader reader(buffer);
    uint8_t status = 0;
    if (!reader.str(out.order_id))
        return false;
    if (!reader.u8(status))
        return false;
    out.status = static_cast<OrderStatus>(status);
    if (!reader.str(out.detail))
        return false;
    if (!reader.u64(out.total_cents))
        return false;
    return reader.done();
}

inline StreamBuffer
encode_inventory_reserve(const InventoryReservePayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    encode_lines(writer, payload.lines);
    return writer.finish();
}

inline bool decode_inventory_reserve(const StreamBuffer& buffer,
                                     InventoryReservePayload& out) {
    BufferReader reader(buffer);
    if (!reader.str(out.order_id))
        return false;
    if (!decode_lines(reader, out.lines))
        return false;
    return reader.done();
}

inline StreamBuffer encode_inventory_reply(const InventoryReplyPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u8(payload.ok ? 1 : 0);
    writer.u64(payload.reservation_id);
    writer.str(payload.detail);
    return writer.finish();
}

inline bool
decode_inventory_reply(const StreamBuffer& buffer, InventoryReplyPayload& out) {
    BufferReader reader(buffer);
    uint8_t ok = 0;
    if (!reader.str(out.order_id))
        return false;
    if (!reader.u8(ok))
        return false;
    out.ok = ok != 0;
    if (!reader.u64(out.reservation_id))
        return false;
    if (!reader.str(out.detail))
        return false;
    return reader.done();
}

inline StreamBuffer
encode_payment_authorize(const PaymentAuthorizePayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.str(payload.customer_id);
    writer.u64(payload.amount_cents);
    writer.u8(static_cast<uint8_t>(payload.scenario));
    return writer.finish();
}

inline bool decode_payment_authorize(const StreamBuffer& buffer,
                                     PaymentAuthorizePayload& out) {
    BufferReader reader(buffer);
    uint8_t scenario = 0;
    if (!reader.str(out.order_id))
        return false;
    if (!reader.str(out.customer_id))
        return false;
    if (!reader.u64(out.amount_cents))
        return false;
    if (!reader.u8(scenario))
        return false;
    out.scenario = static_cast<ScenarioKind>(scenario);
    return reader.done();
}

inline StreamBuffer encode_payment_reply(const PaymentReplyPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u8(payload.ok ? 1 : 0);
    writer.str(payload.authorization_id);
    writer.str(payload.detail);
    return writer.finish();
}

inline bool
decode_payment_reply(const StreamBuffer& buffer, PaymentReplyPayload& out) {
    BufferReader reader(buffer);
    uint8_t ok = 0;
    if (!reader.str(out.order_id))
        return false;
    if (!reader.u8(ok))
        return false;
    out.ok = ok != 0;
    if (!reader.str(out.authorization_id))
        return false;
    if (!reader.str(out.detail))
        return false;
    return reader.done();
}

inline StreamBuffer encode_fulfillment(const FulfillmentPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u64(payload.reservation_id);
    writer.u8(static_cast<uint8_t>(payload.scenario));
    return writer.finish();
}

inline bool
decode_fulfillment(const StreamBuffer& buffer, FulfillmentPayload& out) {
    BufferReader reader(buffer);
    uint8_t scenario = 0;
    if (!reader.str(out.order_id))
        return false;
    if (!reader.u64(out.reservation_id))
        return false;
    if (!reader.u8(scenario))
        return false;
    out.scenario = static_cast<ScenarioKind>(scenario);
    return reader.done();
}

inline StreamBuffer encode_query_order(const QueryOrderPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    return writer.finish();
}

inline bool decode_query_order(const StreamBuffer& buffer, QueryOrderPayload& out) {
    BufferReader reader(buffer);
    return reader.str(out.order_id) && reader.done();
}

inline uint64_t calculate_subtotal(const std::vector<OrderLine>& lines) {
    uint64_t total = 0;
    for (const auto& line : lines)
        total += static_cast<uint64_t>(line.quantity) * line.unit_cents;
    return total;
}

} // namespace hpactor::examples::order_platform
