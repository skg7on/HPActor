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

#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/types/types.hpp>

#include <functional>

namespace hpactor {

class ActorDirectory;
class BackpressureSignalEmitter;

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

    /// \brief Injected dependencies — all fixed at construction.
    struct Config {
        /// \brief Metrics ring buffer for delivery telemetry.
        /// \c nullptr means metrics are disabled.
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics = nullptr;

        /// \brief Fixed wire port for remote backpressure signals.
        /// Points to a stable \c BackpressureSignalEmitter in network state.
        /// \c nullptr when networking is disabled.
        const BackpressureSignalEmitter* wire_port = nullptr;

        /// \brief Actor directory for delivering signals to local senders'
        ///        \c ActorContext handles.  Must not be \c nullptr.
        ActorDirectory* actor_directory = nullptr;

        /// \brief Local endpoint for source identification in signal frames.
        EndPoint endpoint{};
    };

    /// \brief Construct with injected dependencies.
    ///
    /// \param[in] config Injected configuration.  Pointed-to objects must
    ///                   outlive the coordinator.  Pointers may be updated
    ///                   after construction via \c set_*() methods.
    explicit BackpressureCoordinator(Config config);

    /// \brief Destroy the coordinator.  No-op — all resources are owned
    ///        by the injected components.
    ~BackpressureCoordinator();

    // ── Signal emission ──────────────────────────────────────────────

    /// \brief Emit a backpressure signal to a local sender.
    ///
    /// Records a metric event and delivers the signal to the sender's
    /// \c ActorContext::handle_backpressure() handler.
    ///
    /// \param[in] signal The backpressure signal to deliver.
    /// \param[in] state  Current mailbox pressure state for metrics tagging.
    void emit_local_signal(const mailbox::BackpressureSignal& signal,
                           mailbox::MailboxPressureState state);

    /// \brief Emit a backpressure signal to a remote sender.
    ///
    /// Serializes the signal, records a metric event, and sends it via
    /// the injected transport (or test wire sink).
    ///
    /// \param[in] signal The backpressure signal to serialize and send.
    /// \param[in] state  Current mailbox pressure state for metrics tagging.
    void emit_remote_signal(const mailbox::BackpressureSignal& signal,
                            mailbox::MailboxPressureState state);

    // ── Signal handling ──────────────────────────────────────────────

    /// \brief Handle an incoming remote backpressure signal from the wire.
    ///
    /// \param[in] frame WireFrame containing the serialized backpressure
    ///                  signal payload.
    /// \return \c true if the signal was decoded and delivered successfully.
    /// \retval true  Signal was decoded and delivered to the sender.
    /// \retval false Deserialization failed — the frame was silently dropped.
    bool handle_remote_signal(const net::WireFrame& frame);

    /// \brief Deliver a backpressure signal to the sender's \c ActorContext.
    ///
    /// Looks up the sender in the actor directory and calls
    /// \c handle_backpressure() on its context.  No-op when the sender
    /// ID is zero (no sender) or the directory is unavailable.
    ///
    /// \param[in] signal The backpressure signal to deliver locally.
    void deliver_to_sender(const mailbox::BackpressureSignal& signal) const;

    // ── Test support ─────────────────────────────────────────────────

    /// \brief Set a test wire sink that intercepts outbound backpressure
    ///        frames instead of sending them through the transport.
    ///
    /// \param[in] sink Callback invoked with the sender address and
    ///                 encoded frame bytes.  Pass an empty function to
    ///                 clear the sink.
    void set_wire_sink_for_test(WireSink sink);

  private:
    Config config_;
    WireSink wire_sink_for_test_;
};

} // namespace hpactor
