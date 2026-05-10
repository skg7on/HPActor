#pragma once

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string_view>

namespace hpactor::tracing {

enum class SpanKind : uint8_t {
    kInternal,
    kServer,
    kClient,
    kProducer,
    kConsumer,
};

enum class SpanStatus : uint8_t {
    kUnset,
    kOk,
    kError,
};

struct SpanStart {
    std::string_view name;
    SpanKind kind{SpanKind::kInternal};
    TraceContext parent{};
    bool has_parent{false};
    ActorId actor_id{};
    ActorId sender_actor_id{};
    TypeTag type_tag{TypeTag::Invalid};
    MessageId message_id{};
    uint32_t payload_size{0};
};

struct SpanHandle {
    TraceContext context{};
    SpanId parent_span_id{};
    uint64_t start_ns{0};
    SpanKind kind{SpanKind::kInternal};
    ActorId actor_id{};
    ActorId sender_actor_id{};
    TypeTag type_tag{TypeTag::Invalid};
    MessageId message_id{};
    uint32_t payload_size{0};
    bool recording{false};
};

struct SpanRecord {
    TraceId trace_id;
    SpanId span_id;
    SpanId parent_span_id;
    ActorId actor_id;
    ActorId sender_actor_id;
    uint32_t type_tag{0};
    uint64_t message_id{0};
    uint64_t start_ns{0};
    uint64_t end_ns{0};
    uint32_t payload_size{0};
    SpanKind kind{SpanKind::kInternal};
    SpanStatus status{SpanStatus::kUnset};
    uint16_t attribute_mask{0};
};

} // namespace hpactor::tracing
