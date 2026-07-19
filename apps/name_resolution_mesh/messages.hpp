#pragma once

#include <hpactor/msg/type_tag.hpp>
#include <hpactor/adt/stream_buffer.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace apps::name_resolution_mesh {

// Application-level TypeTags in the User range (> 0x1000) to avoid
// conflicts with system tags (0x00–0xFF) and name protocol tags (0x80–0x84).
inline constexpr hpactor::TypeTag kPingTag        = hpactor::TypeTag{0x1100};
inline constexpr hpactor::TypeTag kPingRequestTag = hpactor::TypeTag{0x1101};

/// Response payload from a PingRequest — carries enough metadata to verify
/// the message arrived at the correct remote actor.
struct PongResponse {
    uint32_t    node_id{0};
    std::string service_name;
    int64_t     timestamp_ns{0};
};

/// Encode a PongResponse into a StreamBuffer.
/// Wire format (big-endian):
///   [4 bytes: node_id] [4 bytes: name_len] [name_len bytes: service_name]
///   [8 bytes: timestamp_ns]
inline hpactor::adt::StreamBuffer encode_pong_response(const PongResponse& pong) {
    hpactor::adt::StreamBuffer buf;
    // node_id (big-endian uint32_t)
    buf.push_back(static_cast<uint8_t>((pong.node_id >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((pong.node_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((pong.node_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(pong.node_id & 0xFF));
    // service_name length (big-endian uint32_t)
    uint32_t name_len = static_cast<uint32_t>(pong.service_name.size());
    buf.push_back(static_cast<uint8_t>((name_len >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((name_len >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((name_len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(name_len & 0xFF));
    // service_name bytes
    buf.append(reinterpret_cast<const uint8_t*>(pong.service_name.data()),
               name_len);
    // timestamp_ns (big-endian int64_t)
    uint64_t ts = static_cast<uint64_t>(pong.timestamp_ns);
    buf.push_back(static_cast<uint8_t>((ts >> 56) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ts & 0xFF));
    return buf;
}

/// Decode a PongResponse from a StreamBuffer.
/// Returns a default-constructed PongResponse on malformed input.
inline PongResponse decode_pong_response(const hpactor::adt::StreamBuffer& buf) {
    PongResponse pong;
    if (buf.size() < 16) return pong;  // minimum: 4 + 4 + 0 + 8

    const uint8_t* data = buf.data();
    size_t offset = 0;

    // node_id
    pong.node_id = (static_cast<uint32_t>(data[offset]) << 24) |
                   (static_cast<uint32_t>(data[offset + 1]) << 16) |
                   (static_cast<uint32_t>(data[offset + 2]) << 8) |
                   static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    // service_name length
    uint32_t name_len = (static_cast<uint32_t>(data[offset]) << 24) |
                        (static_cast<uint32_t>(data[offset + 1]) << 16) |
                        (static_cast<uint32_t>(data[offset + 2]) << 8) |
                        static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    // Bounds check
    if (offset + name_len + 8 > buf.size()) return PongResponse{};

    // service_name
    pong.service_name.assign(reinterpret_cast<const char*>(data + offset),
                             name_len);
    offset += name_len;

    // timestamp_ns
    pong.timestamp_ns =
        (static_cast<int64_t>(data[offset]) << 56) |
        (static_cast<int64_t>(data[offset + 1]) << 48) |
        (static_cast<int64_t>(data[offset + 2]) << 40) |
        (static_cast<int64_t>(data[offset + 3]) << 32) |
        (static_cast<int64_t>(data[offset + 4]) << 24) |
        (static_cast<int64_t>(data[offset + 5]) << 16) |
        (static_cast<int64_t>(data[offset + 6]) << 8) |
        static_cast<int64_t>(data[offset + 7]);

    return pong;
}

}  // namespace apps::name_resolution_mesh
