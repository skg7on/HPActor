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

#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/actor/abstract_actor.hpp>
#include <string>

namespace hpactor::metrics {

Aggregator::Aggregator(MetricRegistry& registry, ActorSystem& system)
    : registry_(registry), system_(system) {}

void Aggregator::ensure_families_registered() {
    if (mailbox_depth_family_) return;
    mailbox_depth_family_      = &registry_.register_family(
        "hpactor_mailbox_depth", "Current mailbox queue depth.", MetricType::kGauge);
    mailbox_messages_family_   = &registry_.register_family(
        "hpactor_mailbox_messages_total", "Total messages enqueued.", MetricType::kCounter);
    processing_latency_family_ = &registry_.register_family(
        "hpactor_message_processing_seconds", "Message processing latency.",
        MetricType::kHistogram);
    lifecycle_family_          = &registry_.register_family(
        "hpactor_actor_lifecycle_total", "Actor lifecycle events.", MetricType::kCounter);
    scheduler_dispatch_family_ = &registry_.register_family(
        "hpactor_scheduler_dispatches_total", "Scheduler dispatches.", MetricType::kCounter);
    scheduler_steal_family_    = &registry_.register_family(
        "hpactor_scheduler_steals_total", "Work steals.", MetricType::kCounter);
    supervisor_restart_family_ = &registry_.register_family(
        "hpactor_supervisor_restarts_total", "Actor restarts.", MetricType::kCounter);
    memory_bytes_family_       = &registry_.register_family(
        "hpactor_memory_active_bytes", "Active allocated bytes.", MetricType::kGauge);
}

LabelSet Aggregator::make_actor_labels(ActorId id) {
    LabelSet ls;
    std::string type_name;

    auto it = actor_type_cache_.find(id);
    if (it != actor_type_cache_.end()) {
        type_name = it->second;
    } else {
        auto actor = system_.get_actor(id);
        type_name = actor ? std::string(actor->type_name()) : "unknown";
        actor_type_cache_[id] = type_name;
    }

    ls.labels.emplace_back("actor_id", std::to_string(id.value()));
    ls.labels.emplace_back("actor_type", type_name);
    return ls;
}

void Aggregator::begin_drain() {
    ensure_families_registered();
}

void Aggregator::end_drain() {
    LabelSet ls;
    auto& active_family = registry_.register_family(
        "hpactor_actors_active", "Number of currently active actors.", MetricType::kGauge);
    auto& g = registry_.get_or_create<GaugeValue>(active_family, ls);
    g.value.store(active_actors_, std::memory_order_relaxed);
}

void Aggregator::on_event(const MetricEvent& e) {
    ensure_families_registered();

    switch (e.event_type) {
    case MetricEventType::kMailboxEnqueue: {
        auto lb = make_actor_labels(e.actor_id);
        {
            auto& g = registry_.get_or_create<GaugeValue>(*mailbox_depth_family_, lb);
            g.value.fetch_add(1, std::memory_order_relaxed);
        }
        {
            auto& c = registry_.get_or_create<CounterValue>(*mailbox_messages_family_, lb);
            c.total.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    case MetricEventType::kMailboxDequeue: {
        auto& g = registry_.get_or_create<GaugeValue>(
            *mailbox_depth_family_, make_actor_labels(e.actor_id));
        g.value.fetch_sub(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kMessageProcessed: {
        auto& h = registry_.get_or_create<HistogramValue>(
            *processing_latency_family_, make_actor_labels(e.actor_id));
        h.observe(e.value_hi);
        break;
    }
    case MetricEventType::kActorSpawned:
        active_actors_++;
        [[fallthrough]];
    case MetricEventType::kActorTerminated: {
        if (e.event_type == MetricEventType::kActorTerminated)
            active_actors_--;
        auto& c = registry_.get_or_create<CounterValue>(
            *lifecycle_family_, make_actor_labels(e.actor_id));
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSchedulerDispatch: {
        LabelSet ls;
        ls.labels.emplace_back("worker_id", std::to_string(e.value_hi));
        auto& c = registry_.get_or_create<CounterValue>(*scheduler_dispatch_family_, ls);
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSchedulerSteal: {
        LabelSet ls;
        ls.labels.emplace_back("source_worker", std::to_string(e.value_hi));
        auto& c = registry_.get_or_create<CounterValue>(*scheduler_steal_family_, ls);
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSupervisorRestart: {
        auto& c = registry_.get_or_create<CounterValue>(
            *supervisor_restart_family_, make_actor_labels(e.actor_id));
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kMemoryAlloc: {
        auto& g = registry_.get_or_create<GaugeValue>(
            *memory_bytes_family_, make_actor_labels(e.actor_id));
        g.value.fetch_add(static_cast<int64_t>(e.value_hi), std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kMemoryFree: {
        auto& g = registry_.get_or_create<GaugeValue>(
            *memory_bytes_family_, make_actor_labels(e.actor_id));
        g.value.fetch_sub(static_cast<int64_t>(e.value_hi), std::memory_order_relaxed);
        break;
    }
    }
}

} // namespace hpactor::metrics
