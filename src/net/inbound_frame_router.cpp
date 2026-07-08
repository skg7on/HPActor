// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include "inbound_frame_router.hpp"

#include <hpactor/msg/type_tag.hpp>
#include <hpactor/rpc/rpc_channel.hpp>

#include <hpactor/runtime/messaging_runtime.hpp>

namespace hpactor::net {

namespace {

FrameDispatchResult
make_result(FrameDispatchCode code, WireFrame::PayloadType pt) noexcept {
    FrameDispatchResult r{};
    r.code = code;
    r.payload_type = pt;
    return r;
}

inline bool is_rpc_response(const ActorMsgFrame& data) noexcept {
    return (data.flags() & WireFrame::RpcResponse) != 0;
}

constexpr bool has_flag(uint32_t flags, uint32_t bit) noexcept {
    return (flags & bit) != 0;
}

constexpr bool is_python_binding_control_tag(uint32_t tag) noexcept {
    return tag >= 0xF0u && tag <= 0xF3u;
}

} // namespace

InboundFrameRouter::InboundFrameRouter(Dependencies dependencies, Config config) noexcept
    : config_(config), messaging_(dependencies.messaging), rpc_(dependencies.rpc),
      streams_(dependencies.streams), metrics_(dependencies.metrics) {}

// ── Public API ──────────────────────────────────────────────────────────────

void InboundFrameRouter::disable() noexcept {
    accepting_.store(false, std::memory_order_release);
}

FrameDispatchResult
InboundFrameRouter::on_decode_failure(const InboundFrameContext& /*ictx*/,
                                      FrameDecodeError /*error*/) noexcept {
    return make_result(FrameDispatchCode::DecodeFailed,
                       WireFrame::PayloadType::Unknown);
}

FrameDispatchResult InboundFrameRouter::on_frame(const InboundFrameContext& ictx,
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
            return route_dedicated_ack(frame);
        case WireFrame::PayloadType::Nack:
            return route_dedicated_nack(frame);
        case WireFrame::PayloadType::Batch:
            return route_batch(ictx, frame);
        case WireFrame::PayloadType::StreamOpen: {
            auto sd = streams_.on_open(ictx, frame.pb_envelope.stream_open());
            auto r = make_result(FrameDispatchCode::StreamHandled,
                                 WireFrame::PayloadType::StreamOpen);
            r.detail_code = static_cast<uint32_t>(sd.code);
            r.accepted_count = sd.accepted_count;
            r.rejected_count = sd.rejected_count;
            return r;
        }
        case WireFrame::PayloadType::StreamData: {
            auto sd = streams_.on_data(ictx, frame.pb_envelope.stream_data());
            auto r = make_result(FrameDispatchCode::StreamHandled,
                                 WireFrame::PayloadType::StreamData);
            r.detail_code = static_cast<uint32_t>(sd.code);
            r.accepted_count = sd.accepted_count;
            r.rejected_count = sd.rejected_count;
            return r;
        }
        case WireFrame::PayloadType::StreamAck: {
            auto sd = streams_.on_ack(ictx, frame.pb_envelope.stream_ack());
            auto r = make_result(FrameDispatchCode::StreamHandled,
                                 WireFrame::PayloadType::StreamAck);
            r.detail_code = static_cast<uint32_t>(sd.code);
            return r;
        }
        case WireFrame::PayloadType::StreamClose: {
            auto sd = streams_.on_close(ictx, frame.pb_envelope.stream_close());
            auto r = make_result(FrameDispatchCode::StreamHandled,
                                 WireFrame::PayloadType::StreamClose);
            r.detail_code = static_cast<uint32_t>(sd.code);
            r.accepted_count = sd.accepted_count;
            r.rejected_count = sd.rejected_count;
            return r;
        }
        case WireFrame::PayloadType::StreamError: {
            auto sd = streams_.on_error(ictx, frame.pb_envelope.stream_error());
            auto r = make_result(FrameDispatchCode::StreamHandled,
                                 WireFrame::PayloadType::StreamError);
            r.detail_code = static_cast<uint32_t>(sd.code);
            r.accepted_count = sd.accepted_count;
            r.rejected_count = sd.rejected_count;
            return r;
        }
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

    // ── Reliable control flag disambiguation ──
    // Order MATTERS: dual-bit legacy ACK before single bits.
    const bool ack_req = has_flag(flags, WireFrame::AckRequested);
    const bool ack_resp = has_flag(flags, WireFrame::AckResponse);

    if (ack_req && ack_resp) {
        // Legacy rolling-compatible ACK: both flags set
        messaging_.on_reliable_ack(MessageId{data.message_id()},
                                   net::from_proto(data.sender()).endpoint);
        return make_result(FrameDispatchCode::ReliableAckHandled,
                           WireFrame::PayloadType::Data);
    }
    if (ack_resp) {
        // Legacy NACK: only AckResponse set
        uint32_t reason_code = data.type_tag();
        uint32_t retry_ms = 0;
        if (data.payload().size() >= sizeof(uint32_t)) {
            std::memcpy(&retry_ms, data.payload().data(), sizeof(uint32_t));
        }
        messaging_.on_reliable_nack(MessageId{data.message_id()},
                                    net::from_proto(data.sender()).endpoint,
                                    reason_code, retry_ms);
        return make_result(FrameDispatchCode::ReliableNackHandled,
                           WireFrame::PayloadType::Data);
    }

    // ── Backpressure detection ─────────────────────────────────────────────
    if (static_cast<TypeTag>(data.type_tag()) == TypeTag::BackpressureSignalTag) {
        messaging_.backpressure().handle_remote_signal(frame);
        return make_result(FrameDispatchCode::BackpressureHandled,
                           WireFrame::PayloadType::Data);
    }

    // Ordinary data — AckRequested only (or no control flags).
    // AckRequested flag preserved as message metadata, not consumed.
    return deliver_ordinary_data(ictx, frame, data);
}

// ── Private: Dedicated ACK/NACK oneof routing ───────────────────────────────

FrameDispatchResult
InboundFrameRouter::route_dedicated_ack(const WireFrame& frame) noexcept {
    const auto& ack = frame.pb_envelope.ack_frame();
    messaging_.on_reliable_ack(MessageId{ack.message_id()},
                               net::from_proto(ack.sender()).endpoint);
    return make_result(FrameDispatchCode::ReliableAckHandled,
                       WireFrame::PayloadType::Ack);
}

FrameDispatchResult
InboundFrameRouter::route_dedicated_nack(const WireFrame& frame) noexcept {
    const auto& nack = frame.pb_envelope.nack_frame();
    messaging_.on_reliable_nack(
        MessageId{nack.message_id()}, net::from_proto(nack.sender()).endpoint,
        static_cast<uint32_t>(nack.reason()), nack.retry_after_ms());
    return make_result(FrameDispatchCode::ReliableNackHandled,
                       WireFrame::PayloadType::Nack);
}

// ── Private: Batch routing ─────────────────────────────────────────────────

FrameDispatchResult
InboundFrameRouter::route_batch(const InboundFrameContext& /*ictx*/,
                                const WireFrame& frame) noexcept {
    const auto& batch = frame.pb_envelope.batch_frame();

    // Enforce bounds before iteration
    const int entry_count = batch.entries_size();
    if (entry_count < 0 ||
        static_cast<uint32_t>(entry_count) > config_.max_batch_entries) {
        return make_result(FrameDispatchCode::InvalidFlags,
                           WireFrame::PayloadType::Batch);
    }

    uint32_t accepted = 0;
    uint32_t rejected = 0;

    for (int i = 0; i < entry_count; ++i) {
        const auto& entry = batch.entries(i);

        // Reject Python binding protected control tags in batch entries.
        if (is_python_binding_control_tag(entry.type_tag())) {
            auto r = make_result(FrameDispatchCode::InvalidControlPayload,
                                 WireFrame::PayloadType::Batch);
            r.detail_code = entry.type_tag();
            return r;
        }

        // Reconstruct per-entry TypedMessage
        StreamBuffer payload(entry.payload().begin(), entry.payload().end());
        auto type_tag = static_cast<TypeTag>(entry.type_tag());
        TypedMessage msg(type_tag, std::move(payload));

        // Common sender from batch frame
        msg.set_sender_address(from_proto(batch.sender()));
        msg.set_message_id(entry.message_id());

        if (entry.has_trace_context()) {
            auto parsed = trace_context_from_proto(entry.trace_context(),
                                                   config_.max_tracestate_len);
            if (parsed.has_value()) {
                msg.set_trace_context(parsed.value());
            }
        }

        ActorId target = from_proto(batch.receiver()).id;
        mailbox::DeliveryOptions opts{};
        auto result = messaging_.try_deliver(target, std::move(msg), 0, 0, opts);
        if (result.accepted()) {
            ++accepted;
        } else {
            ++rejected;
        }
    }

    if (rejected == 0 && accepted > 0) {
        auto r = make_result(FrameDispatchCode::BatchDelivered,
                             WireFrame::PayloadType::Batch);
        r.accepted_count = accepted;
        return r;
    }
    if (accepted > 0) {
        auto r = make_result(FrameDispatchCode::BatchPartiallyDelivered,
                             WireFrame::PayloadType::Batch);
        r.accepted_count = accepted;
        r.rejected_count = rejected;
        return r;
    }
    return make_result(FrameDispatchCode::ActorRejected,
                       WireFrame::PayloadType::Batch);
}

// ── Private: Ordinary data delivery ─────────────────────────────────────────

FrameDispatchResult
InboundFrameRouter::deliver_ordinary_data(const InboundFrameContext& /*ictx*/,
                                          const WireFrame& /*frame_outer*/,
                                          const ActorMsgFrame& data) noexcept {
    StreamBuffer payload(data.payload().begin(), data.payload().end());
    auto type_tag = static_cast<TypeTag>(data.type_tag());

    // Reject Python binding protected control tags from remote ingress.
    if (is_python_binding_control_tag(static_cast<uint32_t>(type_tag))) {
        auto r = make_result(FrameDispatchCode::InvalidControlPayload,
                             WireFrame::PayloadType::Data);
        r.detail_code = static_cast<uint32_t>(type_tag);
        return r;
    }

    TypedMessage msg(type_tag, std::move(payload));

    msg.set_sender_address(from_proto(data.sender()));

    if (data.has_trace_context()) {
        auto parsed = trace_context_from_proto(data.trace_context(),
                                               config_.max_tracestate_len);
        if (parsed.has_value()) {
            msg.set_trace_context(parsed.value());
        }
    }

    uint32_t flags = data.flags();
    if (has_flag(flags, WireFrame::AckRequested)) {
        msg.set_ack_requested(true);
    }
    msg.set_message_id(data.message_id());

    auto receiver_addr = from_proto(data.receiver());
    ActorId target = receiver_addr.id;

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
