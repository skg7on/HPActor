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

class Aggregator {
  public:
    Aggregator(MetricRegistry& registry, ActorSystem& system);

    void on_event(const MetricEvent& e);

    void begin_drain();
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

    int64_t active_actors_{0};

    mutable std::unordered_map<ActorId, std::string> actor_type_cache_;

    void ensure_families_registered();
    LabelSet make_actor_labels(ActorId id);
};

} // namespace hpactor::metrics
