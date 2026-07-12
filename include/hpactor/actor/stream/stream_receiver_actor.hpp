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
#include <hpactor/actor/stream/stream_config.hpp>
#include <hpactor/actor/stream/stream_types.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

/// \brief Internal actor that manages the receive side of a streaming session.
///
/// Owns the receive buffer, reassembly, window advertisement, and chunk
/// delivery to the target actor's mailbox. The advertised credit window
/// integrates with the target actor's mailbox pressure.
class StreamReceiverActor : public EventBasedActor {
  public:
    StreamReceiverActor(ActorContext* ctx, ActorSystem& system,
                        ActorId target_actor_id, uint64_t stream_id,
                        ActorAddress sender_addr, EndPoint peer,
                        uint32_t initial_window_bytes,
                        TraceContext trace_ctx);

    void on_activate() override { become(make_behavior()); }

    Behavior make_behavior() override;

  private:
    void handle_stream_data(const ::hpactor::net::StreamDataFrame& data);
    void handle_stream_close(const ::hpactor::net::StreamCloseFrame& close);
    void handle_stream_error(const ::hpactor::net::StreamErrorFrame& error);
    void send_ack();
    uint32_t compute_window_bytes();

    ActorId target_actor_id_;
    uint64_t stream_id_;
    ActorAddress sender_addr_;
    /// Actual TCP peer endpoint (from the accepted connection) for routing
    /// ACKs back via the correct transport pool.
    EndPoint peer_;
    uint32_t initial_window_bytes_;
    uint64_t last_delivered_seq_ = 0;
    uint64_t total_bytes_received_ = 0;
    TraceContext trace_ctx_;
};

} // namespace hpactor
