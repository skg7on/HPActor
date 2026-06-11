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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/metrics/metrics_formatter.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <memory>

namespace hpactor::metrics {

/// \brief System actor that drains the metric ring buffer and serves the
///        /metrics endpoint.
///
/// Registered as a system actor during ActorSystem construction when
/// metrics are enabled. On each /metrics scrape, drains the shared ring
/// buffer through the Aggregator into the MetricRegistry, takes a snapshot,
/// and formats the response via OpenMetricsFormatter.
///
/// \note Thread affinity: runs on the scheduler like any EventBasedActor.
///       Ring buffer writes are lock-free (MPSC); the drain path is
///       single-consumer.
class MetricsActor : public EventBasedActor {
  public:
    /// \brief Construct the metrics actor.
    ///
    /// \param[in] ctx Actor context (always nullptr during construction).
    /// \param[in] system The actor system.
    /// \param[in] ring_buffer Shared metric event ring buffer to drain.
    MetricsActor(ActorContext* ctx, ActorSystem& system,
                 std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer);

    /// \brief Register the /metrics request handler.
    void register_handlers() override;

    /// \brief Format a human-readable metrics snapshot from the current
    ///        ring-buffer state.
    ///
    /// Drains the shared ring buffer through the aggregator, takes a
    /// registry snapshot, and returns a formatted multi-line string.
    /// \return Formatted metrics text, or "No metrics registered yet."
    ///         if the registry is empty.
    std::string format_snapshot();

    /// \brief MetricsActor is always a system actor.
    bool is_system_actor() const override {
        return true;
    }

    /// \brief Actor type name constant.
    static constexpr const char* kActorTypeName = "MetricsActor";

  private:
    std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer_;
    MetricRegistry registry_;
    Aggregator aggregator_;
    OpenMetricsFormatter formatter_;
    /// \brief Snapshot of events_lost from the ring buffer for observability.
    uint64_t events_lost_{0};
};

} // namespace hpactor::metrics
