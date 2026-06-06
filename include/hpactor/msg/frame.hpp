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

    uint32_t magic_hdr = MagicHeader;       ///< Magic header value.
    size_t length;                          ///< Valid after \c encode().
    ::hpactor::net::ActorMsgFrame pb_frame; ///< Protobuf frame body.

    /// \brief Encode the frame to wire format.
    ///
    /// Produces: magic + length + protobuf payload.
    ///
    /// \return A \c StreamBuffer containing the complete wire representation.
    StreamBuffer encode() const;

    /// \brief Decode a frame from a \c StreamBuffer.
    ///
    /// \param[in] data Raw wire-format bytes.
    /// \return A \c WireFrame populated from the decoded data.
    static WireFrame decode(const StreamBuffer& data);

    /// \brief Decode a frame from a byte span (ownership-boundary copy).
    ///
    /// \param[in] data Raw wire-format bytes.
    /// \return A \c WireFrame populated from the decoded data.
    static WireFrame decode(std::span<const uint8_t> data);

    // ── Frame flags ─────────────────────────────────────────────────────

    static constexpr uint32_t Important = 1 << 0;  ///< Priority delivery hint.
    static constexpr uint32_t NoDrop = 1 << 1;     ///< Guaranteed delivery;
                                                   ///< must not be dropped on
                                                   ///< congestion.
    static constexpr uint32_t RpcRequest = 1 << 2; ///< Frame is an RPC request.
    static constexpr uint32_t RpcResponse = 1 << 3;   ///< Frame is an RPC
                                                      ///< response.
    static constexpr uint32_t RpcIdempotent = 1 << 4; ///< Safe to retry;
                                                      ///< receiver should
                                                      ///< deduplicate by
                                                      ///< \c MessageId.
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
