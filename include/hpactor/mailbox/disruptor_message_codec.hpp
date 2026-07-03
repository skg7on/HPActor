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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/mailbox/disruptor_message_envelope.hpp>

#include <cstring>
#include <optional>

namespace hpactor::mailbox {

/// \brief Wire codec for fixed-message Disruptor envelopes.
///
/// Encodes the active variant alternative into a contiguous byte buffer
/// prefixed with a 2-byte type tag. Each \c DisruptorMessage type already
/// satisfies \c std::is_trivially_copyable, so encoding is a direct
/// \c memcpy of the message body.
///
/// \tparam Messages The closed set of fixed user-message types.
template <DisruptorMessage... Messages> class DisruptorMessageCodec {
  public:
    using envelope_type = DisruptorMessageEnvelope<Messages...>;

    /// \brief Encode an envelope into a byte buffer suitable for wire
    ///        transmission.
    ///
    /// Format: [type_tag: uint16_t][envelope.message: N bytes]
    /// where N = sizeof(active variant alternative).
    ///
    /// \param[in] env The envelope to encode.
    /// \return A \c StreamBuffer containing the encoded bytes, or an invalid
    ///         buffer on allocation failure.
    [[nodiscard]] static StreamBuffer encode(const envelope_type& env) noexcept {
        uint16_t tag = static_cast<uint16_t>(env.message.index());
        size_t payload_size = sizeof(envelope_type);
        size_t total = 2 + payload_size;

        auto buf = StreamBuffer::with_capacity(total);
        if (buf.capacity() < total) {
            return {};
        }
        buf.resize(total);

        auto* data = buf.data();
        std::memcpy(data, &tag, sizeof(tag));
        std::memcpy(data + 2, &env, payload_size);
        return buf;
    }

    /// \brief Decode a byte buffer back into an envelope.
    ///
    /// \param[in] data The encoded byte buffer.
    /// \param[in] meta  Envelope metadata to attach to the decoded envelope.
    /// \return The decoded envelope, or \c std::nullopt if the type tag
    ///         is invalid or the buffer is too small.
    [[nodiscard]] static std::optional<envelope_type>
    decode(const StreamBuffer& data, DisruptorEnvelopeMeta meta) noexcept {
        if (data.size() < 2 + sizeof(envelope_type)) {
            return std::nullopt;
        }

        const auto* bytes = data.data();
        uint16_t tag = 0;
        std::memcpy(&tag, bytes, sizeof(tag));

        if (tag >= sizeof...(Messages)) {
            return std::nullopt;
        }

        envelope_type env;
        std::memcpy(&env, bytes + 2, sizeof(envelope_type));

        // Verify the decoded variant index matches the tag.
        if (env.message.index() != static_cast<size_t>(tag)) {
            return std::nullopt;
        }

        // Restore metadata from the caller (wire format does not carry
        // meta — it's reconstructed from the transport layer).
        env.meta = meta;
        return env;
    }
};

} // namespace hpactor::mailbox
