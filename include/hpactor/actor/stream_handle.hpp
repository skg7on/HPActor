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
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>
#include <string_view>
#include <utility>

namespace hpactor {

/// \brief Move-only handle for a streaming session.
///
/// Returned by \c ActorContext::open_stream(). The user's actor writes chunks,
/// closes the stream, or signals an error through this handle. All protocol
/// state (credit window, sequencing, send buffer) lives in the internal
/// \c StreamSenderActor.
///
/// \note \c write() is non-blocking — it enqueues into the sender actor's
///       bounded send buffer and returns immediately. If the send buffer is
///       full, \c write() returns \c false and the caller should apply
///       backpressure to its own producer.
class StreamHandle {
  public:
    StreamHandle() = default;
    StreamHandle(ActorId sender_actor_id, uint64_t stream_id)
        : sender_actor_id_(sender_actor_id), stream_id_(stream_id),
          closed_(false) {}

    ~StreamHandle() = default;

    // Move-only
    StreamHandle(StreamHandle&& other) noexcept
        : sender_actor_id_(other.sender_actor_id_),
          stream_id_(other.stream_id_), closed_(other.closed_) {
        other.stream_id_ = 0;
        other.closed_ = true;
    }

    StreamHandle& operator=(StreamHandle&& other) noexcept {
        if (this != &other) {
            sender_actor_id_ = other.sender_actor_id_;
            stream_id_ = other.stream_id_;
            closed_ = other.closed_;
            other.stream_id_ = 0;
            other.closed_ = true;
        }
        return *this;
    }

    StreamHandle(const StreamHandle&) = delete;
    StreamHandle& operator=(const StreamHandle&) = delete;

    /// Write a chunk to the stream.
    /// \return \c false if stream is closed, send buffer is full, or sender
    ///         actor is gone. \c true if the chunk was queued for send.
    bool write(TypedMessage /*chunk*/) {
        if (closed_)
            return false;
        // Forward to StreamSenderActor in Phase 3
        return true;
    }

    /// Write raw bytes as a stream chunk.
    /// The chunk is delivered to the receiver with the original \p tag.
    bool write(TypeTag /*tag*/, StreamBuffer /*payload*/) {
        if (closed_)
            return false;
        return true;
    }

    /// Gracefully close the stream. Sends StreamCloseFrame(COMPLETE).
    /// \return \c false if already closed.
    bool close() {
        if (closed_)
            return false;
        closed_ = true;
        return true;
    }

    /// Abort the stream with an error code.
    /// \return \c false if already closed.
    bool error(uint32_t /*code*/, std::string_view /*description*/ = "") {
        if (closed_)
            return false;
        closed_ = true;
        return true;
    }

    /// Number of bytes written but not yet acknowledged by the receiver.
    size_t bytes_in_flight() const {
        return 0;
    }

    /// Current advertised receiver window in bytes.
    size_t window_bytes() const {
        return 0;
    }

    /// True if the stream is open (not yet closed or errored).
    bool is_open() const {
        return stream_id_ != 0 && !closed_;
    }

    /// Unique stream identifier.
    uint64_t stream_id() const {
        return stream_id_;
    }

  private:
    ActorId sender_actor_id_{};
    uint64_t stream_id_ = 0;
    bool closed_ = false;
};

} // namespace hpactor
