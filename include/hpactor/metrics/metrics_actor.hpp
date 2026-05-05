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

class MetricsActor : public EventBasedActor {
public:
    MetricsActor(ActorSystem& system,
                 std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer);

    void register_handlers() override;

    static constexpr const char* kActorTypeName = "MetricsActor";

private:
    std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer_;
    MetricRegistry                                registry_;
    Aggregator                                    aggregator_;
    OpenMetricsFormatter                          formatter_;
    uint64_t                                      events_lost_{0};
};

} // namespace hpactor::metrics
