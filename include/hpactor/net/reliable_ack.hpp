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

#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace hpactor {
namespace net {

using Duration = std::chrono::milliseconds;

/// \brief ACK/NACK status for reliable message delivery.
enum class AckStatus : uint8_t {
    Accepted = 0,  ///< Message accepted by receiver.
    Rejected = 1,  ///< Message rejected (receiver may retry after \c
                   ///< retry_after).
    Duplicate = 2, ///< Message is a duplicate (already received).
};

/// \brief Wire payload for an ACK or NACK response.
struct AckPayload {
    MessageId message_id;                    ///< ID of the message being acked.
    AckStatus status = AckStatus::Accepted;  ///< Delivery outcome.
    Duration retry_after = Duration::zero(); ///< Suggested retry delay (NACK
                                             ///< only).
};

/// \brief Encode an \c AckPayload to wire format (14 bytes).
///
/// Wire format: [8 bytes message_id][1 byte status][1 byte reserved][4 bytes
/// retry_after ms]
///
/// \param[in] payload The ACK payload to encode.
/// \return A \c StreamBuffer containing the encoded bytes, or \c std::nullopt
/// on failure.
std::optional<StreamBuffer> encode_ack(const AckPayload& payload);

/// \brief Decode an \c AckPayload from wire format.
///
/// \param[in] data Pointer to the wire-format bytes. Must not be \c nullptr.
/// \param[in] len  Number of available bytes. Must be at least 14.
/// \return The decoded \c AckPayload, or \c std::nullopt if the input is
/// invalid.
std::optional<AckPayload> decode_ack(const uint8_t* data, size_t len);

} // namespace net
} // namespace hpactor
