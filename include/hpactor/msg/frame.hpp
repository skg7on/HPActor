// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>
#include <span>

namespace hpactor {

namespace net {

/// \brief Network message frame — the on-wire representation of a message.
///
/// Every message sent over the network is wrapped in a \c WireFrame.
///
/// Wire format:
/// \code{.txt}
/// [4 bytes: magic "HPAC" (0x43415048 little-endian)]
/// [4 bytes: remaining_length in network byte order]
/// [N bytes: protobuf-serialized ActorMsgFrame]
/// \endcode
///
/// The magic header identifies the stream as HPACTOR protocol. The length
/// prefix enables message boundary detection without parsing protobuf.
///
/// \note Thread safety: \c encode() and \c decode() are free functions that
///       operate on value copies. The struct itself has no internal
///       synchronization.
struct WireFrame {
    /// Magic header identifying HPACTOR framework messages
    /// ("HPAC" in little-endian).
    static constexpr uint32_t MagicHeader = 0x43415048;
    /// Total wire-format header size in bytes (4 magic + 4 length).
    static constexpr size_t HeaderSize = 8;

    uint32_t magic_hdr = MagicHeader;         ///< Magic header value.
    size_t length = 0;                        ///< Valid after \c encode().
    ::hpactor::net::WireEnvelope pb_envelope; ///< Protobuf envelope (oneof).

    /// \brief Encode the envelope to wire format.
    StreamBuffer encode() const;

    /// \brief Decode a frame from a \c StreamBuffer.
    static WireFrame decode(const StreamBuffer& data);

    /// \brief Decode a frame from a byte span.
    static WireFrame decode(std::span<const uint8_t> data);

    // ── Oneof discriminator ─────────────────────────────────────────────

    /// \brief Which oneof field is populated in the envelope.
    enum class PayloadType { Data, Ack, Nack, Batch, Unknown };

    /// \brief Return the payload type based on the oneof discriminator.
    PayloadType payload_type() const {
        switch (pb_envelope.payload_case()) {
            case ::hpactor::net::WireEnvelope::kDataFrame:
                return PayloadType::Data;
            case ::hpactor::net::WireEnvelope::kAckFrame:
                return PayloadType::Ack;
            case ::hpactor::net::WireEnvelope::kNackFrame:
                return PayloadType::Nack;
            case ::hpactor::net::WireEnvelope::kBatchFrame:
                return PayloadType::Batch;
            default:
                return PayloadType::Unknown;
        }
    }

    // ── Convenience factories ──────────────────────────────────────────

    static WireFrame from_data(::hpactor::net::ActorMsgFrame msg) {
        WireFrame f;
        *f.pb_envelope.mutable_data_frame() = std::move(msg);
        return f;
    }

    static WireFrame from_batch(::hpactor::net::BatchMsgFrame batch) {
        WireFrame f;
        *f.pb_envelope.mutable_batch_frame() = std::move(batch);
        return f;
    }

    // ── Frame flags ────────────────────────────────────────────────────

    static constexpr uint32_t Important = 1 << 0;
    static constexpr uint32_t NoDrop = 1 << 1;
    static constexpr uint32_t RpcRequest = 1 << 2;
    static constexpr uint32_t RpcResponse = 1 << 3;
    static constexpr uint32_t RpcIdempotent = 1 << 4;
    static constexpr uint32_t AckRequested = 1 << 5;
    static constexpr uint32_t AckResponse = 1 << 6;
};

// ── Address conversion helpers ─────────────────────────────────────────────

/// \brief Convert C++ \c ActorAddress to protobuf.
///
/// \param[out] pb_addr Destination protobuf address.
/// \param[in] addr Source C++ address.
void to_proto(::hpactor::PbActorAddress* pb_addr, const ActorAddress& addr);

/// \brief Convert C++ \c ActorAddress to protobuf actor reference.
///
/// \param[out] pb_ref Destination protobuf actor ref.
/// \param[in] addr Source C++ address.
void to_proto(::hpactor::PbActorRef* pb_ref, const ActorAddress& addr);

/// \brief Convert protobuf address to C++ \c ActorAddress.
///
/// \param[in] pb_addr Source protobuf address.
/// \return The equivalent C++ \c ActorAddress.
ActorAddress from_proto(const ::hpactor::PbActorAddress& pb_addr);

/// \brief Convert protobuf actor ref to C++ \c ActorAddress.
///
/// \param[in] pb_ref Source protobuf actor ref.
/// \return The equivalent C++ \c ActorAddress.
ActorAddress from_proto(const ::hpactor::PbActorRef& pb_ref);

/// \brief Serialize a \c TraceContext into the protobuf trace context.
///
/// \param[out] pb Destination protobuf trace context.
/// \param[in] context Source trace context.
void to_proto(::hpactor::net::PbTraceContext* pb, const TraceContext& context);

/// \brief Deserialize a protobuf trace context into a \c TraceContext.
///
/// \param[in] pb Source protobuf trace context.
/// \param[in] max_tracestate_len Maximum allowed tracestate string length.
/// \return The deserialized \c TraceContext, or an error if invalid.
result<TraceContext>
trace_context_from_proto(const ::hpactor::net::PbTraceContext& pb,
                         uint16_t max_tracestate_len);

} // namespace net
} // namespace hpactor
