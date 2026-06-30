// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include "inbound_frame_router.hpp"

#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/rpc/rpc_channel.hpp>

#include "runtime/messaging_runtime.hpp"

namespace hpactor::net {

namespace {

FrameDispatchResult
make_result(FrameDispatchCode code, WireFrame::PayloadType pt) noexcept {
    FrameDispatchResult r{};
    r.code = code;
    r.payload_type = pt;
    return r;
}

constexpr bool is_rpc_response(const ActorMsgFrame& data) noexcept {
    return (data.flags() & WireFrame::RpcResponse) != 0;
}

constexpr bool has_flag(uint32_t flags, uint32_t bit) noexcept {
    return (flags & bit) != 0;
}

} // namespace

InboundFrameRouter::InboundFrameRouter(Dependencies dependencies, Config config) noexcept
    : config_(config), messaging_(dependencies.messaging),
      rpc_(dependencies.rpc), metrics_(dependencies.metrics) {}

// ── Public API ──────────────────────────────────────────────────────────────

InboundFrameSink InboundFrameRouter::inbound_sink() noexcept {
    InboundFrameSink sink{};
    sink.context = this;
    sink.route = [](void* ctx, const InboundFrameContext& ictx,
                    const WireFrame& frame) noexcept -> FrameDispatchResult {
        auto* self = static_cast<InboundFrameRouter*>(ctx);
        return self->route(ictx, frame);
    };
    sink.decode_failed = [](void* ctx, const InboundFrameContext& ictx,
                            FrameDecodeError error) noexcept -> FrameDispatchResult {
        auto* self = static_cast<InboundFrameRouter*>(ctx);
        return self->on_decode_failure(ictx, error);
    };
    return sink;
}

void InboundFrameRouter::disable() noexcept {
    accepting_.store(false, std::memory_order_release);
}

FrameDispatchResult
InboundFrameRouter::on_decode_failure(const InboundFrameContext& /*ictx*/,
                                      FrameDecodeError /*error*/) noexcept {
    return make_result(FrameDispatchCode::DecodeFailed,
                       WireFrame::PayloadType::Unknown);
}

FrameDispatchResult InboundFrameRouter::route(const InboundFrameContext& ictx,
                                              const WireFrame& frame) noexcept {
    if (!accepting_.load(std::memory_order_acquire)) {
        return make_result(FrameDispatchCode::RuntimeStopping,
                           frame.payload_type());
    }

    // ── Oneof-first classification ──
    switch (frame.payload_type()) {
        case WireFrame::PayloadType::Data:
            return route_data_payload(ictx, frame);
        case WireFrame::PayloadType::Ack:
        case WireFrame::PayloadType::Nack:
            return make_result(FrameDispatchCode::HandlerUnavailable,
                               frame.payload_type());
        case WireFrame::PayloadType::Batch:
            return make_result(FrameDispatchCode::HandlerUnavailable,
                               frame.payload_type());
        case WireFrame::PayloadType::StreamOpen:
        case WireFrame::PayloadType::StreamData:
        case WireFrame::PayloadType::StreamAck:
        case WireFrame::PayloadType::StreamClose:
        case WireFrame::PayloadType::StreamError:
            return make_result(FrameDispatchCode::HandlerUnavailable,
                               frame.payload_type());
        case WireFrame::PayloadType::Unknown:
        default:
            return make_result(FrameDispatchCode::UnsupportedPayload,
                               frame.payload_type());
    }
}

// ── Private: Data payload routing ───────────────────────────────────────────

FrameDispatchResult
InboundFrameRouter::route_data_payload(const InboundFrameContext& ictx,
                                       const WireFrame& frame) noexcept {
    const auto& data = frame.pb_envelope.data_frame();
    uint32_t flags = data.flags();

    // Reject conflicting RPC/control flags
    if (is_rpc_response(data) && (has_flag(flags, WireFrame::AckRequested) ||
                                  has_flag(flags, WireFrame::AckResponse))) {
        return make_result(FrameDispatchCode::InvalidFlags,
                           WireFrame::PayloadType::Data);
    }

    // RPC response — route directly to RpcChannel
    if (is_rpc_response(data)) {
        RpcResponseFrame rpc_resp{};
        rpc_resp.msg_id = MessageId(data.message_id());
        rpc_resp.payload =
            StreamBuffer(data.payload().begin(), data.payload().end());
        if (data.has_trace_context()) {
            auto parsed = trace_context_from_proto(
                data.trace_context(), config_.rpc_max_tracestate_len);
            if (parsed.has_value()) {
                rpc_resp.has_trace_context = true;
                rpc_resp.trace_context = parsed.value();
            }
        }
        rpc_.on_response(rpc_resp);
        return make_result(FrameDispatchCode::RpcResponseHandled,
                           WireFrame::PayloadType::Data);
    }

    // Ordinary data — deliver through full MessagingRuntime pipeline
    return deliver_ordinary_data(ictx, frame, data);
}

FrameDispatchResult
InboundFrameRouter::deliver_ordinary_data(const InboundFrameContext& /*ictx*/,
                                          const WireFrame& /*frame_outer*/,
                                          const ActorMsgFrame& data) noexcept {
    // Reconstruct TypedMessage from wire format
    StreamBuffer payload(data.payload().begin(), data.payload().end());
    auto type_tag = static_cast<TypeTag>(data.type_tag());
    TypedMessage msg(type_tag, std::move(payload));

    // Set sender address
    msg.set_sender_address(from_proto(data.sender()));

    // Trace context
    if (data.has_trace_context()) {
        auto parsed = trace_context_from_proto(data.trace_context(),
                                               config_.max_tracestate_len);
        if (parsed.has_value()) {
            msg.set_trace_context(parsed.value());
        }
    }

    // Preserve reliable metadata
    uint32_t flags = data.flags();
    if (has_flag(flags, WireFrame::AckRequested)) {
        msg.set_ack_requested(true);
    }
    msg.set_message_id(data.message_id());

    // Extract target actor id
    auto receiver_addr = from_proto(data.receiver());
    ActorId target = receiver_addr.id;

    // Full messaging delivery
    mailbox::DeliveryOptions opts{};
    auto result = messaging_.try_deliver(target, std::move(msg), 0, 0, opts);

    if (result.accepted()) {
        return make_result(FrameDispatchCode::ActorDelivered,
                           WireFrame::PayloadType::Data);
    }
    auto r = make_result(FrameDispatchCode::ActorRejected,
                         WireFrame::PayloadType::Data);
    r.detail_code = static_cast<uint32_t>(result.code);
    return r;
}

} // namespace hpactor::net
