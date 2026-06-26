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
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/types/types.hpp>
#include <vector>

namespace hpactor {

/// \brief Internal actor that manages the send side of a streaming session.
///
/// Owns the send buffer, credit window tracking, chunk sequencing, and idle
/// timeout timer. Communicates with the remote \c StreamReceiverActor via
/// the stream wire protocol.
class StreamSenderActor : public EventBasedActor {
  public:
    StreamSenderActor(ActorSystem& system, ActorId receiver_id, uint64_t stream_id,
                      StreamConfig config, TraceContext trace_ctx);

    Behavior make_behavior() override;

    /// Called by StreamHandle (via message) to enqueue a chunk for send.
    void enqueue_chunk(TypedMessage chunk);

    /// Query methods for StreamHandle
    size_t bytes_in_flight() const {
        return bytes_in_flight_;
    }
    size_t window_bytes() const {
        return window_bytes_;
    }
    bool is_stream_open() const {
        return state_ == State::Streaming;
    }

  private:
    enum class State : uint8_t { Opening, Streaming, Closing, Closed, Error };

    void handle_stream_ack(const ::hpactor::net::StreamAckFrame& ack);
    void handle_stream_close(const ::hpactor::net::StreamCloseFrame& close);
    void handle_stream_error(const ::hpactor::net::StreamErrorFrame& error);
    void send_pending_chunks();
    void on_idle_timeout();

    ActorId receiver_id_;
    uint64_t stream_id_;
    StreamConfig config_;
    TraceContext trace_ctx_;
    State state_ = State::Opening;
    uint32_t window_bytes_ = 0;
    size_t bytes_in_flight_ = 0;
    uint64_t next_sequence_ = 1;
    uint64_t last_acked_ = 0;
    std::vector<TypedMessage> send_buffer_;
    size_t send_buffer_bytes_ = 0;
};

} // namespace hpactor
