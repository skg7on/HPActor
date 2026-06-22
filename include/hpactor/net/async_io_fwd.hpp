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
#include <sys/socket.h>
#include <sys/uio.h>

#include <functional>

namespace hpactor {
namespace net {

/// \brief Categories of asynchronous I/O operations tracked by the event
/// system.
enum class OpType : uint32_t {
    Send = 1,       ///< Asynchronous send on a connected socket.
    Recv = 2,       ///< Asynchronous receive on a connected socket.
    Accept = 3,     ///< Asynchronous accept on a listening socket.
    Connect = 4,    ///< Asynchronous connect completion.
    TimerFired = 5, ///< Timer expiry event.
    RecvFrom = 6,   ///< Asynchronous receive from an unconnected socket.
    SendTo = 7,     ///< Asynchronous send to an unconnected socket.
};

/// \brief Completion record for an asynchronous I/O operation.
///
/// Delivered to the owning actor after the backend finishes (or fails) the
/// requested operation. The \c result field holds the byte count on success,
/// or a negative \c errno value on failure.
///
/// \note Thread safety: Produced by the event loop thread, consumed by the
///       owning actor's scheduler worker.
struct OpCompletion {
    /// \brief Actor that requested the operation.
    ActorId actor;
    /// \brief Type of the completed operation.
    OpType type;
    /// \brief File descriptor the operation was submitted on.
    int fd;
    /// \brief Byte count on success, negative \c errno on failure.
    int result;
    /// \brief Opaque user data (timer handle, etc.).
    uint64_t user_data = 0;
    /// \brief Source address for \c RecvFrom completions.
    sockaddr_in src_addr = {};
    /// \brief Length of \c src_addr.
    socklen_t src_addr_len = 0;
};

/// \brief Callback invoked when a file descriptor becomes readable.
///
/// The callback owns the read: it must \c ::read() from the fd directly.
/// \note Thread safety: Called from the event loop thread.
using read_callback = std::function<void(int fd)>;

/// \brief Callback invoked when a file descriptor becomes writable.
///
/// Used for non-blocking connect completion: the callback checks \c SO_ERROR
/// and transitions the connection to \c Connected on success.
/// \note Thread safety: Called from the event loop thread.
using write_callback = std::function<void(int fd)>;

/// \brief Encode fd, actor, and op_type into a single \c uint64_t for the
/// \c user_data field of an async operation.
///
/// Bit layout: bits 0-31 = fd, bits 32-47 = actor_id, bits 48-55 = reserved,
/// bits 56-63 = op_type.
///
/// \param[in] fd File descriptor.
/// \param[in] actor Target actor.
/// \param[in] op_type Operation type.
/// \return Packed \c user_data value.
inline uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

/// \brief Decode a previously packed \c user_data value.
///
/// \param[in] user_data Packed value from \c encode_user_data().
/// \param[out] fd Decoded file descriptor.
/// \param[out] actor Decoded actor identifier.
/// \param[out] op_type Decoded operation type.
inline void
decode_user_data(uint64_t user_data, int& fd, ActorId& actor, uint32_t& op_type) {
    fd = static_cast<int>(user_data & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((user_data >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((user_data >> 56) & 0xFFULL);
}

/// \brief Unpack an \c OpCompletion from a \c StreamBuffer.
///
/// Used to reconstruct a completion that was serialized via
/// \c ActorSystem::enqueue_completion for cross-thread delivery.
///
/// \param[in] payload Buffer containing the serialized completion.
/// \param[out] out Receives the unpacked completion.
/// \return \c true if the payload size matches \c sizeof(OpCompletion),
///         \c false otherwise.
inline bool unpack_completion(const StreamBuffer& payload, OpCompletion& out) {
    if (payload.size() != sizeof(OpCompletion))
        return false;
    std::memcpy(&out, payload.data(), sizeof(OpCompletion));
    return true;
}

} // namespace net
} // namespace hpactor
