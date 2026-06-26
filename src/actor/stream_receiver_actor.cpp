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
#include <hpactor/actor/stream_receiver_actor.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/frame.hpp>

namespace hpactor {

StreamReceiverActor::StreamReceiverActor(ActorSystem& system,
                                         ActorId target_actor_id, uint64_t stream_id,
                                         ActorAddress sender_addr,
                                         uint32_t initial_window_bytes,
                                         TraceContext trace_ctx)
    : EventBasedActor(nullptr, system), target_actor_id_(target_actor_id),
      stream_id_(stream_id), sender_addr_(std::move(sender_addr)),
      initial_window_bytes_(initial_window_bytes), trace_ctx_(trace_ctx) {}

Behavior StreamReceiverActor::make_behavior() {
    return Behavior{[this](TypedMessage& msg) {
        auto tag = msg.type_id();
        if (tag == stream::StreamDataTag) {
            auto pb = msg.as<::hpactor::net::StreamDataFrame>();
            if (pb)
                handle_stream_data(*pb);
        } else if (tag == stream::StreamCloseTag) {
            auto pb = msg.as<::hpactor::net::StreamCloseFrame>();
            if (pb)
                handle_stream_close(*pb);
        } else if (tag == stream::StreamWireErrorTag) {
            auto pb = msg.as<::hpactor::net::StreamErrorFrame>();
            if (pb)
                handle_stream_error(*pb);
        }
    }};
}

void StreamReceiverActor::handle_stream_data(const ::hpactor::net::StreamDataFrame& data) {
    uint64_t seq = data.sequence();

    // Check ordering: require exactly next-in-sequence.
    if (seq != last_delivered_seq_ + 1) {
        // Send stream error for out-of-order delivery.
        ::hpactor::net::StreamErrorFrame error;
        error.set_stream_id(stream_id_);
        error.set_error_code(2); // OUT_OF_ORDER
        error.set_description("Out-of-order stream sequence");

        TypedMessage err_msg(stream::StreamWireErrorTag, error);
        err_msg.set_sender_address(address());
        system().try_deliver_local_fast(sender_addr_.id, std::move(err_msg));
        return;
    }

    // Deliver chunk to target actor.
    const auto& payload = data.payload();
    StreamBuffer chunk_data(payload.begin(), payload.end());
    TypedMessage chunk(stream::StreamChunkTag, std::move(chunk_data));
    chunk.set_sender_address(sender_addr_);
    if (trace_ctx_.valid()) {
        chunk.set_trace_context(trace_ctx_);
    }

    system().try_deliver_local_fast(target_actor_id_, std::move(chunk));

    last_delivered_seq_ = seq;
    total_bytes_received_ += data.payload().size();

    send_ack();
}

void StreamReceiverActor::send_ack() {
    ::hpactor::net::StreamAckFrame ack;
    ack.set_stream_id(stream_id_);
    ack.set_last_sequence(last_delivered_seq_);
    ack.set_window_bytes(compute_window_bytes());

    TypedMessage ack_msg(stream::StreamAckTag, ack);
    ack_msg.set_sender_address(address());
    system().try_deliver_local_fast(sender_addr_.id, std::move(ack_msg));
}

uint32_t StreamReceiverActor::compute_window_bytes() {
    // Query target actor's mailbox pressure.
    auto target = system().get_actor(target_actor_id_);
    if (!target)
        return 0; // Target gone — close window.

    auto snapshot = target->mailbox_snapshot();
    uint32_t max_window = initial_window_bytes_;
    uint32_t pressure_ppm = snapshot.pressure_ratio_ppm;

    // Window shrinks linearly from 100% at low-watermark (200,000 ppm)
    // to 0% at high-watermark (800,000 ppm).
    if (pressure_ppm <= 200000) {
        return max_window;
    }
    if (pressure_ppm >= 800000) {
        return 0;
    }
    float ratio = 1.0f - static_cast<float>(pressure_ppm - 200000) / 600000.0f;
    return static_cast<uint32_t>(static_cast<float>(max_window) * ratio);
}

void StreamReceiverActor::handle_stream_close(
    const ::hpactor::net::StreamCloseFrame& /*close*/) {
    // Deliver StreamClosedTag to target actor.
    TypedMessage close_msg(stream::StreamClosedTag, StreamBuffer{});
    close_msg.set_sender_address(sender_addr_);
    system().try_deliver_local_fast(target_actor_id_, std::move(close_msg));
}

void StreamReceiverActor::handle_stream_error(
    const ::hpactor::net::StreamErrorFrame& /*error*/) {
    // Deliver StreamErrorTag to target actor.
    TypedMessage err_msg(stream::StreamErrorTag, StreamBuffer{});
    err_msg.set_sender_address(sender_addr_);
    system().try_deliver_local_fast(target_actor_id_, std::move(err_msg));
}

} // namespace hpactor
