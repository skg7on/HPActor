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
    enum class PayloadType : uint8_t {
        Data,
        Ack,
        Nack,
        Batch,
        StreamOpen,
        StreamData,
        StreamAck,
        StreamClose,
        StreamError,
        Unknown
    };

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
            case ::hpactor::net::WireEnvelope::kStreamOpen:
                return PayloadType::StreamOpen;
            case ::hpactor::net::WireEnvelope::kStreamData:
                return PayloadType::StreamData;
            case ::hpactor::net::WireEnvelope::kStreamAck:
                return PayloadType::StreamAck;
            case ::hpactor::net::WireEnvelope::kStreamClose:
                return PayloadType::StreamClose;
            case ::hpactor::net::WireEnvelope::kStreamError:
                return PayloadType::StreamError;
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

    static WireFrame from_ack(::hpactor::net::AckFrame ack) {
        WireFrame f;
        *f.pb_envelope.mutable_ack_frame() = std::move(ack);
        return f;
    }

    static WireFrame from_nack(::hpactor::net::NackFrame nack) {
        WireFrame f;
        *f.pb_envelope.mutable_nack_frame() = std::move(nack);
        return f;
    }

    static WireFrame from_stream_open(::hpactor::net::StreamOpenFrame open) {
        WireFrame f;
        *f.pb_envelope.mutable_stream_open() = std::move(open);
        return f;
    }

    static WireFrame from_stream_data(::hpactor::net::StreamDataFrame data) {
        WireFrame f;
        *f.pb_envelope.mutable_stream_data() = std::move(data);
        return f;
    }

    static WireFrame from_stream_ack(::hpactor::net::StreamAckFrame ack) {
        WireFrame f;
        *f.pb_envelope.mutable_stream_ack() = std::move(ack);
        return f;
    }

    static WireFrame from_stream_close(::hpactor::net::StreamCloseFrame close) {
        WireFrame f;
        *f.pb_envelope.mutable_stream_close() = std::move(close);
        return f;
    }

    static WireFrame from_stream_error(::hpactor::net::StreamErrorFrame error) {
        WireFrame f;
        *f.pb_envelope.mutable_stream_error() = std::move(error);
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

// ── Strict decode types ──────────────────────────────────────────────────────

/// \brief Specific reason a \c WireFrame decode failed.
///
/// Production paths use \c try_decode() and match on the error code.
/// The legacy \c decode() returns an empty frame on any failure.
enum class FrameDecodeError : uint8_t {
    None = 0,        ///< No error; the frame is valid.
    HeaderTooShort,  ///< Fewer than 8 bytes; cannot parse magic or length.
    InvalidMagic,    ///< First 4 bytes are not \c "HPAC".
    FrameTooLarge,   ///< Declared payload exceeds \c max_payload_bytes.
    LengthMismatch,  ///< Buffer shorter than declared payload length.
    TrailingBytes,   ///< Extra bytes after the declared frame (when reject
                     ///< enabled).
    InvalidProtobuf, ///< Payload did not parse as a valid \c WireEnvelope.
};

/// \brief Bounds that control \c try_decode() validation.
///
/// Defaults match the existing outbound byte bound (16 MiB) so a
/// receiver that picks the defaults does not silently drop valid frames.
struct FrameDecodeLimits {
    /// Maximum allowed protobuf payload bytes. 0 means check is disabled.
    uint32_t max_payload_bytes{16U * 1024U * 1024U};
    /// When true, exactly \c HeaderSize + payload_len bytes are required.
    bool reject_trailing_bytes{true};
};

/// \brief Result of a strict wire-frame decode.
///
/// Owns the decoded \c WireFrame when \c ok() is true.  Callers can
/// distinguish every framing and protobuf failure without inspecting
/// the returned frame's oneof discriminator.
struct FrameDecodeResult {
    WireFrame frame; ///< Valid only when \c ok().
    FrameDecodeError error{FrameDecodeError::None};
    uint32_t declared_payload_bytes{0}; ///< Value from the wire length field.

    /// \brief True when decoding succeeded with no validation errors.
    [[nodiscard]] bool ok() const noexcept {
        return error == FrameDecodeError::None;
    }
};

// ── WireFrame strict decode (declared after FrameDecodeResult exists) ────

/// \brief Strict frame decode with typed error result.
///
/// Validates magic, size bounds, and protobuf integrity before
/// constructing a \c WireFrame.  Returns a \c FrameDecodeResult
/// that owns the decoded frame on success or carries a specific
/// \c FrameDecodeError on failure.
///
/// \param data  The encoded bytes (header + protobuf payload).
/// \param limits  Validation bounds; defaults to 16 MiB, trailing reject.
/// \return A result whose \c ok() is true only on clean decode.
FrameDecodeResult
try_decode_wireframe(const StreamBuffer& data, FrameDecodeLimits limits = {});

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
