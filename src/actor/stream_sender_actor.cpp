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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/stream_sender_actor.hpp>
#include <hpactor/msg/frame.hpp>

namespace hpactor {

StreamSenderActor::StreamSenderActor(ActorSystem& system, ActorId receiver_id,
                                     uint64_t stream_id, StreamConfig config,
                                     TraceContext trace_ctx)
    : EventBasedActor(nullptr, system), receiver_id_(receiver_id),
      stream_id_(stream_id), config_(std::move(config)), trace_ctx_(trace_ctx) {
    send_buffer_.reserve(config_.max_in_flight_frames);
}

Behavior StreamSenderActor::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        auto tag = msg.type_id();
        if (tag == stream::StreamAckTag) {
            auto pb = msg.as<::hpactor::net::StreamAckFrame>();
            if (pb)
                handle_stream_ack(*pb);
        } else if (tag == stream::StreamCloseTag) {
            auto pb = msg.as<::hpactor::net::StreamCloseFrame>();
            if (pb)
                handle_stream_close(*pb);
        } else if (tag == stream::StreamWireErrorTag) {
            auto pb = msg.as<::hpactor::net::StreamErrorFrame>();
            if (pb)
                handle_stream_error(*pb);
        } else {
            // User chunk from StreamHandle — enqueue
            enqueue_chunk(std::move(msg));
        }
    }};
}

void StreamSenderActor::enqueue_chunk(TypedMessage chunk) {
    if (state_ != State::Streaming && state_ != State::Opening)
        return;
    size_t chunk_size = chunk.payload().size();
    if (send_buffer_bytes_ + chunk_size > config_.send_buffer_bytes)
        return;
    send_buffer_.push_back(std::move(chunk));
    send_buffer_bytes_ += chunk_size;
    send_pending_chunks();
}

void StreamSenderActor::send_pending_chunks() {
    while (!send_buffer_.empty() && state_ != State::Error) {
        // Credit window check: pause if in-flight exceeds advertised window.
        // window_bytes_ == 0 means the receiver has not yet opened the window.
        // Allow one chunk through for anti-deadlock (initial open).
        if (window_bytes_ > 0 && bytes_in_flight_ >= window_bytes_)
            break;
        if (bytes_in_flight_ > 0 && window_bytes_ == 0)
            break;
        if (state_ != State::Streaming && state_ != State::Opening)
            break;

        TypedMessage chunk = std::move(send_buffer_.front());
        send_buffer_.erase(send_buffer_.begin());
        size_t chunk_size = chunk.payload().size();
        send_buffer_bytes_ -= chunk_size;

        // Build StreamDataFrame
        ::hpactor::net::StreamDataFrame data_frame;
        data_frame.set_stream_id(stream_id_);
        data_frame.set_sequence(next_sequence_++);
        data_frame.set_payload(reinterpret_cast<const char*>(chunk.payload().data()),
                               chunk.payload().size());

        // Serialize proto to TypedMessage and deliver to receiver actor.
        TypedMessage wire_msg(stream::StreamDataTag, data_frame);
        wire_msg.set_sender_address(address());
        if (trace_ctx_.valid()) {
            wire_msg.set_trace_context(trace_ctx_);
        }
        system().try_deliver_local_fast(receiver_id_, std::move(wire_msg));

        bytes_in_flight_ += chunk_size;
    }
}

void StreamSenderActor::handle_stream_ack(const ::hpactor::net::StreamAckFrame& ack) {
    if (ack.last_sequence() > last_acked_) {
        last_acked_ = ack.last_sequence();
    }
    window_bytes_ = ack.window_bytes();

    if (state_ == State::Opening) {
        state_ = State::Streaming;
    }

    if (state_ == State::Streaming) {
        send_pending_chunks();
    }
}

void StreamSenderActor::handle_stream_close(
    const ::hpactor::net::StreamCloseFrame& /*close*/) {
    state_ = State::Closed;
}

void StreamSenderActor::handle_stream_error(
    const ::hpactor::net::StreamErrorFrame& /*error*/) {
    state_ = State::Error;
}

void StreamSenderActor::on_idle_timeout() {
    if (state_ == State::Streaming || state_ == State::Opening) {
        ::hpactor::net::StreamErrorFrame error;
        error.set_stream_id(stream_id_);
        error.set_error_code(1); // TIMEOUT
        error.set_description("Stream idle timeout");

        TypedMessage err_msg(stream::StreamWireErrorTag, error);
        err_msg.set_sender_address(address());
        system().try_deliver_local_fast(receiver_id_, std::move(err_msg));

        state_ = State::Error;
    }
}

} // namespace hpactor
