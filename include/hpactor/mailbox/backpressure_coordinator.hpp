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

#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/types/types.hpp>

#include <functional>

namespace hpactor {

class ActorDirectory;

namespace net {
class EventLoop;
class TcpTransport;
struct WireFrame;
} // namespace net

/// \brief Coordinates system-wide backpressure signal emission and handling.
///
/// Owns the logic for emitting local and remote backpressure signals,
/// including metrics instrumentation, serialization, and wire transport.
/// Extracted from \c ActorSystem. Structured logging is kept in ActorSystem
/// wrapper methods so they can use the HPACTOR_LOG_WARNING macro.
///
/// \note Thread safety: All injected pointers must outlive this coordinator.
///       Signal routing is synchronous and may be called from any thread.
class BackpressureCoordinator {
  public:
    using WireSink = std::function<bool(const ActorAddress&, const StreamBuffer&)>;

    /// \brief Injected dependencies.
    ///
    /// Pointers may be null — the coordinator safely skips metrics emission
    /// and wire transport when the corresponding pointer is null.  Pointers
    /// can be updated after construction via the \c set_*() methods (used
    /// during \c ActorSystem constructor ordering).
    struct Config {
        /// \brief Opaque pointer to a
        ///        \c metrics::MpscRingBuffer<metrics::MetricEvent>.
        ///
        /// Stored as \c void* to avoid including the heavyweight metrics
        /// ring-buffer header (which would create a circular dependency
        /// through its \c using alias).  The implementation casts back to
        /// the concrete type.  Set via \c set_metrics_ring_buffer().
        /// \pre Must point to a valid ring buffer, or be \c nullptr.
        void* metrics_ring_buffer = nullptr;

        /// \brief Transport for sending backpressure signals to remote
        ///        producers.  May be \c nullptr when networking is disabled.
        net::TcpTransport* transport = nullptr;

        /// \brief Actor directory for delivering signals to local senders'
        ///        \c ActorContext handles.  Must not be \c nullptr.
        ActorDirectory* actor_directory = nullptr;

        /// \brief Local endpoint for source identification in signal frames.
        EndPoint endpoint{};
    };

    /// \brief Construct with injected dependencies.
    explicit BackpressureCoordinator(Config config);
    ~BackpressureCoordinator();

    // ── Signal emission ──────────────────────────────────────────────

    void emit_local_signal(const mailbox::BackpressureSignal& signal,
                           mailbox::MailboxPressureState state);

    void emit_remote_signal(const mailbox::BackpressureSignal& signal,
                            mailbox::MailboxPressureState state);

    // ── Signal handling ──────────────────────────────────────────────

    /// \brief Handle an incoming remote backpressure signal from the wire.
    /// \return true if the signal was decoded and delivered successfully.
    bool handle_remote_signal(const net::WireFrame& frame);

    /// \brief Deliver a backpressure signal to the sender's ActorContext.
    void deliver_to_sender(const mailbox::BackpressureSignal& signal);

    // ── Test support ─────────────────────────────────────────────────

    void set_wire_sink_for_test(WireSink sink);

    /// \brief Update the metrics ring buffer pointer after construction.
    ///
    /// The pointer is stored opaquely as \c void* to avoid including the
    /// metrics ring-buffer header.  The implementation casts back to
    /// \c metrics::MpscRingBuffer<metrics::MetricEvent>* at each use site.
    /// Callers must pass the concrete pointer type; the cast is safe
    /// because there is exactly one metrics ring buffer type in the system.
    ///
    /// \param[in] metrics Pointer to the metrics ring buffer, or \c nullptr.
    void set_metrics_ring_buffer(void* metrics) noexcept {
        config_.metrics_ring_buffer = metrics;
    }

    /// \brief Update the transport pointer after construction.
    ///
    /// \param[in] transport Pointer to the TCP transport, or \c nullptr
    ///                      when networking is disabled.
    void set_transport(net::TcpTransport* transport) noexcept {
        config_.transport = transport;
    }

  private:
    Config config_;
    WireSink wire_sink_for_test_;
};

} // namespace hpactor
