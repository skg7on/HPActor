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

#include <atomic>
#include <cstdint>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>
#include <memory>
#include <vector>

namespace hpactor {

/// \brief Internal actor that manages the send side of a streaming session.
///
/// Owns the send buffer, credit window tracking, chunk sequencing, and idle
/// timeout timer. Communicates with the remote \c StreamReceiverActor via
/// the stream wire protocol.
class StreamSenderActor : public EventBasedActor {
  public:
    /// \brief Construct a stream sender.
    /// \param ctx Actor context from the spawning system.
    /// \param system The actor system.
    /// \param receiver_id Target receiver actor ID.
    /// \param receiver_addr Address of the receiver (for remote delivery).
    /// \param stream_id Unique stream identifier.
    /// \param config Stream configuration.
    /// \param trace_ctx Stream-level trace context.
    /// \param is_local True if receiver is on the same node (fast path).
    /// \param state Optional shared state for StreamHandle observability.
    StreamSenderActor(ActorContext* ctx, ActorSystem& system, ActorId receiver_id,
                      ActorAddress receiver_addr, uint64_t stream_id,
                      StreamConfig config, TraceContext trace_ctx, bool is_local,
                      std::shared_ptr<StreamSenderState> state = nullptr);

    Behavior make_behavior() override;

    /// Called by StreamHandle (via message) to enqueue a chunk for send.
    void enqueue_chunk(TypedMessage chunk);

    /// Query methods for StreamHandle (snapshot reads from shared atomics).
    size_t bytes_in_flight() const {
        return shared_state_
                   ? shared_state_->bytes_in_flight->load(std::memory_order_acquire)
                   : 0;
    }
    size_t window_bytes() const {
        return shared_state_
                   ? shared_state_->window_bytes->load(std::memory_order_acquire)
                   : 0;
    }
    bool is_stream_open() const {
        return state_ == State::Streaming;
    }

  private:
    enum class State : uint8_t { Opening, Streaming, Closing, Closed, Error };

    void handle_stream_ack(const ::hpactor::net::StreamAckFrame& ack);
    void handle_stream_close(const ::hpactor::net::StreamCloseFrame& close);
    void handle_stream_error(const ::hpactor::net::StreamErrorFrame& error);
    void handle_internal_close();
    void handle_internal_error(TypedMessage& msg);
    void handle_internal_timeout();
    void send_pending_chunks();
    void send_close_to_receiver();
    void send_error_to_receiver(uint32_t error_code, std::string_view description);
    void schedule_idle_timer();
    void reset_idle_timer();
    void on_idle_timeout();

    ActorId receiver_id_;
    ActorAddress receiver_addr_;
    uint64_t stream_id_;
    StreamConfig config_;
    TraceContext trace_ctx_;
    bool is_local_ = true;
    State state_ = State::Opening;
    /// Shared atomic state for StreamHandle observability.
    std::shared_ptr<StreamSenderState> shared_state_;
    uint64_t next_sequence_ = 1;
    uint64_t last_acked_ = 0;
    std::vector<TypedMessage> send_buffer_;
    size_t send_buffer_bytes_ = 0;
    AlarmHandle idle_timer_handle_;
};

} // namespace hpactor
