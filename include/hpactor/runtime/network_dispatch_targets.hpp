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
#include <hpactor/net/frame_dispatch_result.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor::net {

struct WireFrame;           ///< Full definition in <hpactor/msg/frame.hpp>
struct InboundFrameContext; ///< Full definition in
                            ///< <hpactor/net/inbound_frame_sink.hpp>
struct Member;   ///< Full definition in <hpactor/net/service_discovery.hpp>
class Transport; ///< Full definition in <hpactor/net/tcp_transport.hpp>
enum class FrameDecodeError : uint8_t; ///< Full definition in
                                       ///< <hpactor/msg/frame.hpp>

} // namespace hpactor::net

namespace hpactor {

// ── InboundFrame dispatch ─────────────────────────────────────────────────

/// \brief Typed target for inbound frame delivery (Phase 4 contract).
///
/// Implemented by \c InboundFrameRouter; stored by \c ConnectionPool via
/// \c InboundFrameSink.  Virtual dispatch replaces the previous \c void*
/// context + function-pointer pattern.
class InboundFrameTarget {
  public:
    virtual ~InboundFrameTarget() = default;

    /// \brief Route a successfully decoded frame.
    ///
    /// \param ictx Frame metadata (peer, encoded byte count).
    /// \param frame The decoded wire frame.
    /// \return Fixed-size diagnostic suitable for observability and
    ///         future peer policy.
    virtual net::FrameDispatchResult
    on_frame(const net::InboundFrameContext& ictx,
             const net::WireFrame& frame) noexcept = 0;

    /// \brief Handle a decode failure with the typed error reason.
    ///
    /// Does not attempt recovery or resynchronisation — connection
    /// framing is the caller's responsibility.
    virtual net::FrameDispatchResult
    on_decode_failure(const net::InboundFrameContext& ictx,
                      net::FrameDecodeError error) noexcept = 0;
};

// ── Node membership dispatch ──────────────────────────────────────────────

/// \brief Typed target for node membership events.
///
/// Callbacks fire on the network loop thread. The target must outlive the
/// discovery subscription and must not be destroyed until callbacks are
/// quiesced (subscription removed + last callback returned).
class NodeEventTarget {
  public:
    virtual ~NodeEventTarget() = default;

    /// \brief Called when a node joins or leaves the cluster.
    ///
    /// \param member The node that changed membership status.
    /// \param joined \c true if the node joined, \c false if it left.
    virtual void
    on_member_changed(const net::Member& member, bool joined) noexcept = 0;
};

// ── Reliable retry dispatch ───────────────────────────────────────────────

/// \brief Typed target for reliable-retry processing.
///
/// Called from the network loop thread at a fixed poll interval.
/// The target (MessagingRuntime) must outlive NetworkRuntime.
class OutboundRetryTarget {
  public:
    virtual ~OutboundRetryTarget() = default;

    /// \brief Process due retries.
    ///
    /// \param now_ns Monotonic-clock timestamp in nanoseconds.
    virtual void process_due(uint64_t now_ns) noexcept = 0;
};

// ── Remote spawn dispatch ─────────────────────────────────────────────────

/// \brief Typed target for remote-spawn receiver lifecycle.
///
/// Called from the network component during startup and stop.
/// The target (ActorRuntime) owns the spawn receiver actor and its
/// directory entry; NetworkRuntime owns only the protocol registration.
class RemoteSpawnTarget {
  public:
    virtual ~RemoteSpawnTarget() = default;

    /// \brief Install the remote-spawn receiver actor into the directory
    ///        and register spawn protocol handlers with the transport.
    /// \return The actor address on success, or an error code.
    virtual result<ActorAddress>
    install_receiver(net::Transport& transport) noexcept = 0;

    /// \brief Remove the spawn receiver's protocol registration.
    virtual void remove_receiver() noexcept = 0;
};

// ── Reliable ACK/NACK dispatch ────────────────────────────────────────────

/// \brief Typed target for reliable ACK/NACK frame emission.
///
/// Crosses the \c NetworkRuntime boundary without capturing \c ActorSystem
/// or creating a circular dependency.  Implemented by the entity that owns
/// the network transport state.
class ReliableAckTarget {
  public:
    virtual ~ReliableAckTarget() = default;

    /// \brief Emit a reliable ACK or NACK frame.
    ///
    /// \param target        Destination address for the ACK/NACK frame.
    /// \param acker         Local endpoint that generated the ACK.
    /// \param message_id    Message being acknowledged.
    /// \param status        ACK status: 0=Accepted, 1=Rejected, 2=Duplicate.
    /// \param retry_after_ms Suggested retry delay for NACK (0 if unused).
    virtual void send_ack(const ActorAddress& target, const ActorAddress& acker,
                          uint64_t message_id, uint8_t status,
                          uint32_t retry_after_ms) noexcept = 0;
};

// ── Backpressure signal dispatch ──────────────────────────────────────────

/// \brief Typed target for remote backpressure signal emission.
///
/// Same design as \c ReliableAckTarget: typed interface replacing the
/// \c void* context + function-pointer pattern.
class BackpressureSignalTarget {
  public:
    virtual ~BackpressureSignalTarget() = default;

    /// \brief Send a pre-encoded backpressure signal to a remote node.
    ///
    /// \param target  Destination address.
    /// \param encoded Pre-encoded backpressure frame bytes.
    /// \return \c true if the frame was sent (or queued), \c false if
    ///         unavailable.
    virtual bool send_signal(const ActorAddress& target,
                             const StreamBuffer& encoded) noexcept = 0;
};

} // namespace hpactor
