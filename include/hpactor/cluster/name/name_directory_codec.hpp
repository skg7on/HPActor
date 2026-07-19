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

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/name_directory_tags.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor::cluster::name::codec {

// ── Protobuf wire-format constants ──────────────────────────────────────────
// See: https://protobuf.dev/programming-guides/encoding/
// These constants describe the binary encoding used by protobuf wire format
// and are stable across all protobuf versions.

namespace pb {
// Wire types (3-bit, lower bits of each tag)
inline constexpr uint8_t kVarint          = 0;
inline constexpr uint8_t kFixed64         = 1;
inline constexpr uint8_t kLengthDelimited = 2;
inline constexpr uint8_t kFixed32         = 5;

// Tag decoding
inline constexpr uint8_t  kTagWireTypeMask     = 0x07;
inline constexpr uint8_t  kTagFieldNumberShift = 3;

// Varint encoding
inline constexpr uint8_t  kVarintContinuationBit = 0x80;
inline constexpr uint64_t kVarintPayloadMask     = 0x7FULL;
inline constexpr int      kVarintShiftIncrement  = 7;
inline constexpr int      kVarintMaxShift        = 64;

// Fixed-size field widths (bytes)
inline constexpr size_t kFixed64Size = 8;
inline constexpr size_t kFixed32Size = 4;
} // namespace pb

// ── Field numbers from name_directory.proto ────────────────────────────────
// Messages and their field assignments:
//   PbNameRegisterRequest:   name=1, actor_id=2, endpoint=3, generation=4
//   PbNameRegisterResponse:  result=1
//   PbNameResolveQuery:      name=1
//   PbNameResolveResponse:   found=1, actor_id=2, endpoint=3, generation=4
//   PbNameUnregisterRequest: name=1, generation=2
namespace name_dir {
inline constexpr uint32_t kFieldName       = 1;
inline constexpr uint32_t kFieldActorId    = 2;
inline constexpr uint32_t kFieldEndpoint   = 3;
inline constexpr uint32_t kFieldGeneration = 4;
inline constexpr uint32_t kUnregFieldGeneration = 2;
inline constexpr uint32_t kFieldResult     = 1;
inline constexpr uint32_t kFieldFound      = 1;
} // namespace name_dir

// ── Varint decoding ─────────────────────────────────────────────────────────

/// \brief Decode a single protobuf varint from the byte stream.
///
/// Advances \p p past the decoded bytes.  Returns \c false on truncation
/// or overflow; \c true on success (even for zero).
///
/// \param[in,out] p  Pointer into the byte buffer; advanced past the varint.
/// \param[in]     end One-past-the-end of the buffer.
/// \param[out]    out Decoded unsigned varint value.
/// \return \c true on success, \c false if the input is truncated or the
///         varint would exceed 64 bits.
inline bool decode_varint(const uint8_t*& p, const uint8_t* end,
                          uint64_t& out) noexcept {
    out = 0;
    int shift = 0;
    while (p < end) {
        uint64_t byte = *p++;
        out |= (byte & pb::kVarintPayloadMask) << shift;
        if ((byte & pb::kVarintContinuationBit) == 0) return true;
        shift += pb::kVarintShiftIncrement;
        if (shift >= pb::kVarintMaxShift) return false;
    }
    return false;
}

/// \brief Skip a single protobuf field value of known wire type.
///
/// Advances \p p past the skipped bytes.  Returns \c false on truncation.
inline bool skip_field_value(const uint8_t*& p, const uint8_t* end,
                             uint8_t wire_type) noexcept {
    switch (wire_type) {
    case pb::kVarint:
        while (p < end && (*p & pb::kVarintContinuationBit)) ++p;
        if (p < end) ++p;
        return true;
    case pb::kLengthDelimited: {
        uint64_t len;
        if (!decode_varint(p, end, len)) return false;
        if (len > static_cast<uint64_t>(end - p)) return false;
        p += len;
        return true;
    }
    case pb::kFixed64:
        if (static_cast<ptrdiff_t>(pb::kFixed64Size) > (end - p)) return false;
        p += pb::kFixed64Size;
        return true;
    case pb::kFixed32:
        if (static_cast<ptrdiff_t>(pb::kFixed32Size) > (end - p)) return false;
        p += pb::kFixed32Size;
        return true;
    default:
        return false;
    }
}

// ── Varint encoding ─────────────────────────────────────────────────────────

/// \brief Append a single protobuf varint to the output string.
inline void append_varint(std::string& out, uint64_t value) {
    while (value >= pb::kVarintContinuationBit) {
        out.push_back(static_cast<char>((value & pb::kVarintPayloadMask) |
                                         pb::kVarintContinuationBit));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value & pb::kVarintPayloadMask));
}

/// \brief Append a protobuf tag (field_number | wire_type) as a varint.
inline void append_tag(std::string& out, uint32_t field_number,
                       uint8_t wire_type) {
    append_varint(out, (static_cast<uint64_t>(field_number) <<
                        pb::kTagFieldNumberShift) | wire_type);
}

/// \brief Append a length-delimited field (tag + length + bytes).
inline void append_length_delimited(std::string& out, uint32_t field_number,
                                    std::string_view data) {
    append_tag(out, field_number, pb::kLengthDelimited);
    append_varint(out, data.size());
    out.append(data);
}

/// \brief Append a varint-valued field (tag + varint).
inline void append_varint_field(std::string& out, uint32_t field_number,
                                uint64_t value) {
    append_tag(out, field_number, pb::kVarint);
    append_varint(out, value);
}

// ── Name-directory message encoders ─────────────────────────────────────────

/// \brief Encode a PbNameRegisterRequest into raw proto bytes.
inline std::string encode_register_request(std::string_view name,
                                           uint64_t actor_id,
                                           std::string_view endpoint_str,
                                           uint64_t generation) {
    std::string out;
    append_length_delimited(out, name_dir::kFieldName, name);
    append_varint_field(out, name_dir::kFieldActorId, actor_id);
    append_length_delimited(out, name_dir::kFieldEndpoint, endpoint_str);
    append_varint_field(out, name_dir::kFieldGeneration, generation);
    return out;
}

/// \brief Encode a PbNameRegisterResponse into raw proto bytes.
inline std::string encode_register_response(uint32_t result_code) {
    std::string out;
    append_varint_field(out, name_dir::kFieldResult, result_code);
    return out;
}

/// \brief Encode a PbNameResolveQuery into raw proto bytes.
inline std::string encode_resolve_query(std::string_view name) {
    std::string out;
    append_length_delimited(out, name_dir::kFieldName, name);
    return out;
}

/// \brief Encode a PbNameResolveResponse into raw proto bytes.
inline std::string encode_resolve_response(bool found, uint64_t actor_id,
                                           std::string_view endpoint_str,
                                           uint64_t generation) {
    std::string out;
    append_varint_field(out, name_dir::kFieldFound, found ? 1ULL : 0ULL);
    if (found) {
        append_varint_field(out, name_dir::kFieldActorId, actor_id);
        append_length_delimited(out, name_dir::kFieldEndpoint, endpoint_str);
        append_varint_field(out, name_dir::kFieldGeneration, generation);
    }
    return out;
}

/// \brief Encode a PbNameUnregisterRequest into raw proto bytes.
inline std::string encode_unregister_request(std::string_view name,
                                             uint64_t generation) {
    std::string out;
    append_length_delimited(out, name_dir::kFieldName, name);
    append_varint_field(out, name_dir::kUnregFieldGeneration, generation);
    return out;
}

// ── Convenience ─────────────────────────────────────────────────────────────

/// \brief Build a TypedMessage from a protobuf-encoded payload string.
///
/// Centralises the StreamBuffer::from_data + reinterpret_cast pattern
/// used by every outbound name-protocol send site.
inline TypedMessage make_name_message(TypeTag tag,
                                      const std::string& proto_bytes) {
    StreamBuffer payload = StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(proto_bytes.data()),
        proto_bytes.size());
    return TypedMessage(tag, std::move(payload));
}

} // namespace hpactor::cluster::name::codec
