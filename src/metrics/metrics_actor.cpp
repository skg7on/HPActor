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

#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/common.pb.h>
#include <cstdio>

namespace hpactor::metrics {

MetricsActor::MetricsActor(ActorSystem& system,
                           std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer)
    : EventBasedActor(nullptr, system)
    , ring_buffer_(std::move(ring_buffer))
    , aggregator_(registry_, system) {}

void MetricsActor::register_handlers() {
    on_request<MetricsRequest, MetricsResponse>(
        [this](const MetricsRequest& /*req*/) -> MetricsResponse {
            aggregator_.begin_drain();
            ring_buffer_->drain([this](const MetricEvent& e) {
                aggregator_.on_event(e);
                return true;
            });
            aggregator_.end_drain();

            events_lost_ += ring_buffer_->events_lost();

            auto snapshot = registry_.snapshot();
            std::string body = formatter_.format(snapshot);

            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "# HELP hpactor_metrics_events_lost_total Events lost due to ring buffer overflow.\n"
                "# TYPE hpactor_metrics_events_lost_total counter\n"
                "hpactor_metrics_events_lost_total %llu\n",
                static_cast<unsigned long long>(events_lost_));
            body.append(buf, static_cast<size_t>(n));

            MetricsResponse resp;
            resp.set_body(body);
            return resp;
        });
}

} // namespace hpactor::metrics
