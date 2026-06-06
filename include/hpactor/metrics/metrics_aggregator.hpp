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
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <string>
#include <unordered_map>
namespace hpactor {
class ActorSystem;
} // namespace hpactor

namespace hpactor::metrics {

/// \brief Event-to-metric dispatch engine.
///
/// Consumes MetricEvents from the ring buffer (via on_event()) and updates
/// the corresponding counters, gauges, and histograms in the MetricRegistry.
/// Maintains an actor-type label cache for efficient label resolution.
///
/// \note Thread affinity: owned by MetricsActor, drained on the actor's
///       scheduler thread. Not safe for concurrent access from multiple
///       threads.
class Aggregator {
  public:
    /// \brief Construct an aggregator.
    ///
    /// \param[in] registry Metric registry to update.
    /// \param[in] system Actor system for actor type resolution.
    Aggregator(MetricRegistry& registry, ActorSystem& system);

    /// \brief Process a single metric event.
    ///
    /// Dispatches based on event_type, updating the appropriate counter,
    /// gauge, or histogram in the registry.
    ///
    /// \param[in] e The metric event to process.
    void on_event(const MetricEvent& e);

    /// \brief Signal the start of a drain cycle.
    ///
    /// Called before the ring buffer is drained to allow pre-drain
    /// state capture.
    void begin_drain();

    /// \brief Signal the end of a drain cycle.
    ///
    /// Called after the ring buffer is fully drained.
    void end_drain();

  private:
    MetricRegistry& registry_;
    ActorSystem& system_;

    MetricFamily* mailbox_depth_family_ = nullptr;
    MetricFamily* mailbox_messages_family_ = nullptr;
    MetricFamily* processing_latency_family_ = nullptr;
    MetricFamily* lifecycle_family_ = nullptr;
    MetricFamily* scheduler_dispatch_family_ = nullptr;
    MetricFamily* scheduler_steal_family_ = nullptr;
    MetricFamily* supervisor_restart_family_ = nullptr;
    MetricFamily* memory_bytes_family_ = nullptr;
    MetricFamily* mailbox_rejected_family_ = nullptr;
    MetricFamily* mailbox_dropped_family_ = nullptr;
    MetricFamily* mailbox_dead_letter_family_ = nullptr;
    MetricFamily* backpressure_signal_family_ = nullptr;
    MetricFamily* dead_letter_lost_family_ = nullptr;
    MetricFamily* quarantine_total_family_ = nullptr;
    MetricFamily* unquarantine_total_family_ = nullptr;
    MetricFamily* circuit_state_family_ = nullptr;
    MetricFamily* circuit_trips_family_ = nullptr;

    // Endpoint outbound queue metric families
    MetricFamily* endpoint_outbound_messages_family_ = nullptr;
    MetricFamily* endpoint_outbound_bytes_family_ = nullptr;
    MetricFamily* endpoint_pressure_state_family_ = nullptr;
    MetricFamily* endpoint_circuit_state_family_ = nullptr;
    MetricFamily* endpoint_send_accepted_family_ = nullptr;
    MetricFamily* endpoint_send_rejected_family_ = nullptr;
    MetricFamily* endpoint_backpressure_signals_family_ = nullptr;
    MetricFamily* endpoint_circuit_transitions_family_ = nullptr;
    MetricFamily* delivery_results_family_ = nullptr;

    int64_t active_actors_{0};

    /// \brief LRU-style cache mapping ActorId to actor type name for label
    ///        resolution.
    mutable std::unordered_map<ActorId, std::string> actor_type_cache_;

    /// \brief Register all known metric families in the registry.
    void ensure_families_registered();

    /// \brief Build a LabelSet for a given actor.
    ///
    /// \param[in] id Actor identifier.
    /// \return LabelSet with actor_id and actor_type labels populated.
    LabelSet make_actor_labels(ActorId id);

    /// \brief Build a LabelSet for a given endpoint.
    ///
    /// The endpoint identity is encoded in the \p id.value() field as a
    /// packed IPv4 address and port (addr<<16|port) for IPv4, or a hash
    /// for IPv6. A cache is maintained to avoid repeated string conversion.
    ///
    /// \param[in] id ActorId carrying the packed endpoint identity.
    /// \return LabelSet with an "endpoint" label.
    LabelSet make_endpoint_labels(ActorId id);

    /// \brief Cache mapping packed endpoint identity to endpoint label string.
    mutable std::unordered_map<uint64_t, std::string> endpoint_label_cache_;
};

} // namespace hpactor::metrics
