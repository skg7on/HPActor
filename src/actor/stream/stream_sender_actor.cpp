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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/actor/stream/stream_sender_actor.hpp>
#include <hpactor/msg/frame.hpp>

#include <cstring>

namespace hpactor {

StreamSenderActor::StreamSenderActor(ActorContext* ctx, ActorSystem& system,
                                     ActorId receiver_id, ActorAddress receiver_addr,
                                     uint64_t stream_id, StreamConfig config,
                                     TraceContext trace_ctx, bool is_local,
                                     std::shared_ptr<StreamSenderState> state)
    : EventBasedActor(ctx, system), receiver_id_(receiver_id),
      receiver_addr_(std::move(receiver_addr)), stream_id_(stream_id),
      config_(std::move(config)), trace_ctx_(trace_ctx), is_local_(is_local),
      shared_state_(state ? std::move(state)
                          : std::make_shared<StreamSenderState>()) {
    send_buffer_.reserve(config_.max_in_flight_frames);
}

Behavior StreamSenderActor::make_behavior() {
    // Start the idle timeout watcher.
    schedule_idle_timer();

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
        } else if (tag == stream::InternalCloseTag) {
            handle_internal_close();
        } else if (tag == stream::InternalErrorTag) {
            handle_internal_error(msg);
        } else if (tag == stream::InternalTimeoutTag) {
            handle_internal_timeout();
        } else if (static_cast<uint32_t>(tag) >=
                   static_cast<uint32_t>(TypeTag::User)) {
            // User chunk from StreamHandle — enqueue
            enqueue_chunk(std::move(msg));
        }
        // else: unknown system-range tag — silently drop.
        // Guards against future control tags being misinterpreted as data.
    }};
}

void StreamSenderActor::enqueue_chunk(TypedMessage chunk) {
    // Accept chunks in Opening, Streaming, or Closing state.  The close
    // message (InternalCloseTag, a system-lane message) may be dequeued
    // before remaining user-lane chunks due to system-lane priority.
    // We must accept those chunks even after the close has been processed
    // so they can be sent when ACKs open the credit window.
    if (state_ != State::Streaming && state_ != State::Opening &&
        state_ != State::Closing)
        return;
    size_t chunk_size = chunk.payload().size();
    if (send_buffer_bytes_ + chunk_size > config_.send_buffer_bytes)
        return;
    send_buffer_.push_back(std::move(chunk));
    send_buffer_bytes_ += chunk_size;
    reset_idle_timer();
    send_pending_chunks();
}

void StreamSenderActor::send_pending_chunks() {
    bool sent_any = false;
    while (!send_buffer_.empty() && state_ != State::Error) {
        // Credit window check: pause if in-flight exceeds advertised window.
        // window == 0 means the receiver has not yet opened the window.
        // Allow one chunk through for anti-deadlock (initial open).
        {
            auto cur_window =
                shared_state_->window_bytes->load(std::memory_order_acquire);
            auto cur_inflight =
                shared_state_->bytes_in_flight->load(std::memory_order_acquire);
            if (cur_window > 0 && cur_inflight >= cur_window)
                break;
            if (cur_inflight > 0 && cur_window == 0)
                break;
        }
        // Allow send in Opening, Streaming, and Closing states.
        // Closing is needed so pending chunks can be sent when ACKs
        // arrive after the close has been deferred (system-lane close
        // message dequeued before user-lane chunks).
        if (state_ != State::Streaming && state_ != State::Opening &&
            state_ != State::Closing)
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
        data_frame.set_user_tag(static_cast<uint32_t>(chunk.type_id()));

        if (is_local_) {
            // Local fast path: TypedMessage direct to receiver actor's mailbox.
            TypedMessage wire_msg(stream::StreamDataTag, data_frame);
            wire_msg.set_sender_address(address());
            if (trace_ctx_.valid()) {
                wire_msg.set_trace_context(trace_ctx_);
            }
            system().try_deliver_local_fast(receiver_id_, std::move(wire_msg));
        } else {
            // Remote path: serialize to WireFrame and send via transport.
            auto wire_frame =
                net::WireFrame::from_stream_data(std::move(data_frame));
            auto* tp = system().transport();
            if (tp) {
                (void)tp->try_send(receiver_addr_, wire_frame.encode());
            }
        }

        shared_state_->bytes_in_flight->fetch_add(chunk_size,
                                                  std::memory_order_release);
        sent_any = true;
    }

    // Complete a deferred close only if we actually sent something in
    // this call — proving that user-lane chunks have been dequeued and
    // added to the buffer.  If the buffer was already empty when this
    // was called (handle_internal_close() ran before pending user-lane
    // chunks were dequeued), we don't close — subsequent calls from
    // enqueue_chunk() or handle_stream_ack() will drain the buffer and
    // trigger the close when sent_any is true.
    if (sent_any && state_ == State::Closing && send_buffer_.empty()) {
        send_close_to_receiver();
        state_ = State::Closed;
    }
}

void StreamSenderActor::handle_stream_ack(const ::hpactor::net::StreamAckFrame& ack) {
    if (ack.last_sequence() > last_acked_) {
        last_acked_ = ack.last_sequence();
    }
    shared_state_->window_bytes->store(ack.window_bytes(),
                                       std::memory_order_release);

    if (state_ == State::Opening) {
        state_ = State::Streaming;
    }

    reset_idle_timer();

    // Also drain pending chunks when in Closing state — an ACK may have
    // just opened the window after handle_internal_close() deferred the
    // close frame because the send buffer wasn't empty.
    if (state_ == State::Streaming || state_ == State::Closing) {
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

void StreamSenderActor::handle_internal_close() {
    if (state_ != State::Streaming && state_ != State::Opening)
        return;

    // Transition to Closing state.  Do NOT send the close frame yet:
    // the close message (InternalCloseTag, system lane) may have been
    // dequeued before remaining user-lane chunks due to system-lane
    // priority.  If bytes_in_flight > 0, chunks were sent and more
    // user-lane messages may be pending — defer the close to
    // send_pending_chunks() after subsequent ACKs and enqueue_chunk()
    // calls drain the buffer.  If nothing was ever sent, close now.
    state_ = State::Closing;
    send_pending_chunks();

    if (send_buffer_.empty() &&
        shared_state_->bytes_in_flight->load(std::memory_order_acquire) == 0) {
        // No data ever sent — safe to close immediately.
        send_close_to_receiver();
        state_ = State::Closed;
    }
}

void StreamSenderActor::handle_internal_error(TypedMessage& msg) {
    if (state_ == State::Closed || state_ == State::Error)
        return;

    const auto& payload = msg.payload();
    uint32_t error_code = 0;
    std::string_view description;

    if (payload.size() >= sizeof(uint32_t)) {
        std::memcpy(&error_code, payload.data(), sizeof(uint32_t));
        if (payload.size() > sizeof(uint32_t)) {
            description = std::string_view(
                reinterpret_cast<const char*>(payload.data() + sizeof(uint32_t)),
                payload.size() - sizeof(uint32_t));
        }
    }

    send_error_to_receiver(error_code, description);
    state_ = State::Error;
}

void StreamSenderActor::handle_internal_timeout() {
    if (state_ == State::Streaming || state_ == State::Opening) {
        on_idle_timeout();
    }
}

void StreamSenderActor::send_close_to_receiver() {
    ::hpactor::net::StreamCloseFrame close;
    close.set_stream_id(stream_id_);
    close.set_reason(::hpactor::net::StreamCloseFrame::COMPLETE);

    if (is_local_) {
        TypedMessage close_msg(stream::StreamCloseTag, close);
        close_msg.set_sender_address(address());
        system().try_deliver_local_fast(receiver_id_, std::move(close_msg));
    } else {
        auto wire_frame = net::WireFrame::from_stream_close(std::move(close));
        auto* tp = system().transport();
        if (tp) {
            (void)tp->try_send(receiver_addr_, wire_frame.encode());
        }
    }
}

void StreamSenderActor::send_error_to_receiver(uint32_t error_code,
                                               std::string_view description) {
    ::hpactor::net::StreamErrorFrame error;
    error.set_stream_id(stream_id_);
    error.set_error_code(error_code);
    if (!description.empty()) {
        error.set_description(description.data(), description.size());
    }

    if (is_local_) {
        TypedMessage err_msg(stream::StreamWireErrorTag, error);
        err_msg.set_sender_address(address());
        system().try_deliver_local_fast(receiver_id_, std::move(err_msg));
    } else {
        auto wire_frame = net::WireFrame::from_stream_error(std::move(error));
        auto* tp = system().transport();
        if (tp) {
            (void)tp->try_send(receiver_addr_, wire_frame.encode());
        }
    }
}

void StreamSenderActor::schedule_idle_timer() {
    if (config_.idle_timeout.count() == 0)
        return;
    TypedMessage timeout_msg(stream::InternalTimeoutTag, StreamBuffer{});
    idle_timer_handle_ = context()->schedule(config_.idle_timeout.as_chrono(),
                                             std::move(timeout_msg));
}

void StreamSenderActor::reset_idle_timer() {
    if (idle_timer_handle_.valid()) {
        context()->cancel_schedule(idle_timer_handle_);
    }
    schedule_idle_timer();
}

void StreamSenderActor::on_idle_timeout() {
    if (state_ == State::Streaming || state_ == State::Opening) {
        send_error_to_receiver(1, "Stream idle timeout");
        state_ = State::Error;
    }
}

} // namespace hpactor
