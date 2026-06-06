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

#include <arpa/inet.h>
#include <cstdio>
#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/types/types.hpp>
#include <string>

namespace hpactor::metrics {

Aggregator::Aggregator(MetricRegistry& registry, ActorSystem& system)
    : registry_(registry), system_(system) {}

void Aggregator::ensure_families_registered() {
    if (mailbox_depth_family_)
        return;
    mailbox_depth_family_ = &registry_.register_family("hpactor_mailbox_depth",
                                                       "Current mailbox queue "
                                                       "depth.",
                                                       MetricType::kGauge);
    mailbox_messages_family_ = &registry_.register_family("hpactor_mailbox_"
                                                          "messages_total",
                                                          "Total messages "
                                                          "enqueued.",
                                                          MetricType::kCounter);
    processing_latency_family_ = &registry_.register_family(
        "hpactor_message_processing_seconds", "Message processing latency.",
        MetricType::kHistogram);
    lifecycle_family_ = &registry_.register_family("hpactor_actor_lifecycle_"
                                                   "total",
                                                   "Actor lifecycle events.",
                                                   MetricType::kCounter);
    scheduler_dispatch_family_ =
        &registry_.register_family("hpactor_scheduler_dispatches_total",
                                   "Scheduler dispatches.", MetricType::kCounter);
    scheduler_steal_family_ = &registry_.register_family(
        "hpactor_scheduler_steals_total", "Work steals.", MetricType::kCounter);
    supervisor_restart_family_ =
        &registry_.register_family("hpactor_supervisor_restarts_total",
                                   "Actor restarts.", MetricType::kCounter);
    memory_bytes_family_ = &registry_.register_family("hpactor_memory_active_"
                                                      "bytes",
                                                      "Active allocated bytes.",
                                                      MetricType::kGauge);
    mailbox_rejected_family_ = &registry_.register_family("hpactor_mailbox_"
                                                          "rejected_total",
                                                          "Mailbox admission "
                                                          "rejections.",
                                                          MetricType::kCounter);
    mailbox_dropped_family_ = &registry_.register_family("hpactor_mailbox_"
                                                         "dropped_total",
                                                         "Mailbox policy "
                                                         "drops.",
                                                         MetricType::kCounter);
    mailbox_dead_letter_family_ = &registry_.register_family("hpactor_mailbox_"
                                                             "dead_letters_"
                                                             "total",
                                                             "Mailbox messages "
                                                             "routed to dead "
                                                             "letters.",
                                                             MetricType::kCounter);
    backpressure_signal_family_ = &registry_.register_family("hpactor_"
                                                             "backpressure_"
                                                             "signals_total",
                                                             "Backpressure "
                                                             "signals emitted.",
                                                             MetricType::kCounter);
    dead_letter_lost_family_ = &registry_.register_family("hpactor_dead_letter_"
                                                          "lost_total",
                                                          "Dead-letter records "
                                                          "lost.",
                                                          MetricType::kCounter);
    quarantine_total_family_ = &registry_.register_family("hpactor_actor_"
                                                          "quarantine_total",
                                                          "Actor quarantine "
                                                          "transitions.",
                                                          MetricType::kCounter);
    unquarantine_total_family_ = &registry_.register_family("hpactor_actor_"
                                                            "unquarantine_"
                                                            "total",
                                                            "Actor "
                                                            "unquarantine "
                                                            "transitions.",
                                                            MetricType::kCounter);
    circuit_state_family_ = &registry_.register_family("hpactor_actor_circuit_"
                                                       "state",
                                                       "Per-actor circuit "
                                                       "breaker state "
                                                       "(0=closed, 1=open, "
                                                       "2=half-open).",
                                                       MetricType::kGauge);
    circuit_trips_family_ = &registry_.register_family("hpactor_actor_circuit_"
                                                       "trips_total",
                                                       "Total circuit breaker "
                                                       "trip events.",
                                                       MetricType::kCounter);

    // ── Endpoint outbound queue metric families ─────────────────────
    endpoint_outbound_messages_family_ =
        &registry_.register_family("hpactor_endpoint_outbound_messages",
                                   "Current endpoint outbound queue message "
                                   "count per lane.",
                                   MetricType::kGauge);
    endpoint_outbound_bytes_family_ =
        &registry_.register_family("hpactor_endpoint_outbound_bytes",
                                   "Current endpoint outbound queue byte "
                                   "count per lane.",
                                   MetricType::kGauge);
    endpoint_pressure_state_family_ =
        &registry_.register_family("hpactor_endpoint_pressure_state",
                                   "Endpoint outbound queue pressure state "
                                   "(0=normal, 1=soft, 2=hard).",
                                   MetricType::kGauge);
    endpoint_circuit_state_family_ =
        &registry_.register_family("hpactor_endpoint_circuit_state",
                                   "Endpoint circuit breaker state "
                                   "(0=closed, 1=open, 2=half-open).",
                                   MetricType::kGauge);
    endpoint_send_accepted_family_ =
        &registry_.register_family("hpactor_endpoint_send_accepted_total",
                                   "Total messages accepted into endpoint "
                                   "outbound queue.",
                                   MetricType::kCounter);
    endpoint_send_rejected_family_ =
        &registry_.register_family("hpactor_endpoint_send_rejected_total",
                                   "Total messages rejected by endpoint "
                                   "outbound queue.",
                                   MetricType::kCounter);
    endpoint_backpressure_signals_family_ =
        &registry_.register_family("hpactor_endpoint_backpressure_signals_"
                                   "sent_total",
                                   "Total backpressure signals sent for "
                                   "endpoint outbound queue.",
                                   MetricType::kCounter);
    endpoint_circuit_transitions_family_ =
        &registry_.register_family("hpactor_endpoint_circuit_transitions_"
                                   "total",
                                   "Total endpoint circuit breaker "
                                   "transitions.",
                                   MetricType::kCounter);
    delivery_results_family_ = &registry_.register_family("hpactor_delivery_"
                                                          "results_total",
                                                          "Total delivery "
                                                          "results by status.",
                                                          MetricType::kCounter);
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

LabelSet Aggregator::make_endpoint_labels(ActorId id) {
    LabelSet ls;
    uint64_t packed = id.value();
    auto it = endpoint_label_cache_.find(packed);
    if (it != endpoint_label_cache_.end()) {
        ls.labels.emplace_back("endpoint", it->second);
        return ls;
    }

    // Unpack: for IPv4, the address is in the upper 32 bits and port in the
    // lower 16 bits of packed.
    uint32_t addr = static_cast<uint32_t>(packed >> 16);
    uint16_t port_nw = static_cast<uint16_t>(packed & 0xFFFF);

    // Skip label for unknown/zero-packed endpoints
    if (addr == 0 && port_nw == 0) {
        ls.labels.emplace_back("endpoint", "unknown");
        endpoint_label_cache_[packed] = "unknown";
        return ls;
    }

    struct in_addr in;
    in.s_addr = addr;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s:%u", ::inet_ntoa(in),
                  static_cast<unsigned>(net_to_host_u16(port_nw)));
    std::string label(buf);
    endpoint_label_cache_[packed] = label;
    ls.labels.emplace_back("endpoint", label);
    return ls;
}

void Aggregator::begin_drain() {
    ensure_families_registered();
}

void Aggregator::end_drain() {
    LabelSet ls;
    auto& active_family = registry_.register_family("hpactor_actors_active",
                                                    "Number of currently "
                                                    "active actors.",
                                                    MetricType::kGauge);
    auto& g = registry_.get_or_create<GaugeValue>(active_family, ls);
    g.value.store(active_actors_, std::memory_order_relaxed);
}

void Aggregator::on_event(const MetricEvent& e) {
    FAULT_INJECT("hpactor.metrics.aggregator.on_event.corrupt") {
        return; // silently drop metric event
    }
    ensure_families_registered();

    switch (e.event_type) {
        case MetricEventType::kMailboxEnqueue: {
            auto lb = make_actor_labels(e.actor_id);
            {
                auto& g =
                    registry_.get_or_create<GaugeValue>(*mailbox_depth_family_, lb);
                g.value.fetch_add(1, std::memory_order_relaxed);
            }
            {
                auto& c = registry_.get_or_create<CounterValue>(
                    *mailbox_messages_family_, lb);
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
            auto& c = registry_.get_or_create<CounterValue>(
                *scheduler_dispatch_family_, ls);
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kSchedulerSteal: {
            LabelSet ls;
            ls.labels.emplace_back("source_worker", std::to_string(e.value_hi));
            auto& c =
                registry_.get_or_create<CounterValue>(*scheduler_steal_family_, ls);
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
            g.value.fetch_add(static_cast<int64_t>(e.value_hi),
                              std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kMemoryFree: {
            auto& g = registry_.get_or_create<GaugeValue>(
                *memory_bytes_family_, make_actor_labels(e.actor_id));
            g.value.fetch_sub(static_cast<int64_t>(e.value_hi),
                              std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kMailboxRejected: {
            auto& c = registry_.get_or_create<CounterValue>(
                *mailbox_rejected_family_, make_actor_labels(e.actor_id));
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kMailboxDropped: {
            auto& c = registry_.get_or_create<CounterValue>(
                *mailbox_dropped_family_, make_actor_labels(e.actor_id));
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kMailboxDeadLetter: {
            auto& c = registry_.get_or_create<CounterValue>(
                *mailbox_dead_letter_family_, make_actor_labels(e.actor_id));
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kBackpressureSignal: {
            auto& c = registry_.get_or_create<CounterValue>(
                *backpressure_signal_family_, make_actor_labels(e.actor_id));
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kDeadLetterLost: {
            auto& c = registry_.get_or_create<CounterValue>(
                *dead_letter_lost_family_, make_actor_labels(e.actor_id));
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kLifecycleTransition:
        case MetricEventType::kMessageRejected:
        case MetricEventType::kActorDrainStart:
        case MetricEventType::kActorDrainComplete:
        case MetricEventType::kActorDrainTimeout:
        case MetricEventType::kDeliveryFailure:
        case MetricEventType::kDeliveryDuplicate:
        case MetricEventType::kDeliveryExpired:
            break;
        case MetricEventType::kDeliveryResult: {
            auto labels = make_actor_labels(e.actor_id);
            labels.labels.emplace_back(
                "status",
                hpactor::mailbox::to_string(
                    static_cast<hpactor::mailbox::DeliveryStatus>(e.code)));
            auto& c = registry_.get_or_create<CounterValue>(
                *delivery_results_family_, labels);
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kActorQuarantined: {
            auto& c = registry_.get_or_create<CounterValue>(
                *quarantine_total_family_, make_actor_labels(e.actor_id));
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kActorUnquarantined: {
            auto& c = registry_.get_or_create<CounterValue>(
                *unquarantine_total_family_, make_actor_labels(e.actor_id));
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kFaultInjected:
            break;
        // ── Endpoint outbound queue events ─────────────────────────
        case MetricEventType::kEndpointSendAccepted: {
            auto lb = make_endpoint_labels(e.actor_id);
            std::string mode = (e.code == 0) ? "best_effort" : "at_least_once";
            lb.labels.emplace_back("mode", mode);
            auto& c = registry_.get_or_create<CounterValue>(
                *endpoint_send_accepted_family_, lb);
            c.total.fetch_add(e.value_hi, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kEndpointSendRejected: {
            auto lb = make_endpoint_labels(e.actor_id);
            std::string reason = (e.code == 0) ? "queue_full" : "other";
            lb.labels.emplace_back("reason", reason);
            auto& c = registry_.get_or_create<CounterValue>(
                *endpoint_send_rejected_family_, lb);
            c.total.fetch_add(e.value_hi, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kEndpointOutboundMessages: {
            auto lb = make_endpoint_labels(e.actor_id);
            std::string lane = (e.code == 0) ? "data" : "control";
            lb.labels.emplace_back("lane", lane);
            auto& g = registry_.get_or_create<GaugeValue>(
                *endpoint_outbound_messages_family_, lb);
            g.value.store(static_cast<int64_t>(e.value_hi),
                          std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kEndpointOutboundBytes: {
            auto lb = make_endpoint_labels(e.actor_id);
            std::string lane = (e.code == 0) ? "data" : "control";
            lb.labels.emplace_back("lane", lane);
            auto& g = registry_.get_or_create<GaugeValue>(
                *endpoint_outbound_bytes_family_, lb);
            g.value.store(static_cast<int64_t>(e.value_hi),
                          std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kEndpointPressureState: {
            auto lb = make_endpoint_labels(e.actor_id);
            auto& g = registry_.get_or_create<GaugeValue>(
                *endpoint_pressure_state_family_, lb);
            g.value.store(static_cast<int64_t>(e.code), std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kEndpointCircuitState: {
            auto lb = make_endpoint_labels(e.actor_id);
            auto& g = registry_.get_or_create<GaugeValue>(
                *endpoint_circuit_state_family_, lb);
            g.value.store(static_cast<int64_t>(e.code), std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kEndpointBackpressureSignal: {
            auto lb = make_endpoint_labels(e.actor_id);
            auto& c = registry_.get_or_create<CounterValue>(
                *endpoint_backpressure_signals_family_, lb);
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kEndpointCircuitTransition: {
            auto lb = make_endpoint_labels(e.actor_id);
            auto& c = registry_.get_or_create<CounterValue>(
                *endpoint_circuit_transitions_family_, lb);
            c.total.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MetricEventType::kCircuitStateChange: {
            {
                auto& g = registry_.get_or_create<GaugeValue>(
                    *circuit_state_family_, make_actor_labels(e.actor_id));
                g.value.store(static_cast<int64_t>(e.code),
                              std::memory_order_relaxed);
            }
            {
                auto& c = registry_.get_or_create<CounterValue>(
                    *circuit_trips_family_, make_actor_labels(e.actor_id));
                c.total.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case MetricEventType::kRateLimitBlocked:
        case MetricEventType::kAdmissionRejected:
        case MetricEventType::kAdmissionDLQRouted:
        case MetricEventType::kPerSenderBucketCount:
            // Full metric handlers will be wired in Phase 6 (Metrics & CLI).
            // The event is still emitted into the ring buffer for telemetry.
            break;
        case MetricEventType::kAskSent:
        case MetricEventType::kAskCompleted:
        case MetricEventType::kAskTimeout:
        case MetricEventType::kAskExpired:
        case MetricEventType::kAskRetry:
        case MetricEventType::kAskCancelled:
            // Ask lifecycle metrics — handlers wired in ACT-007.
            break;
    }
}

} // namespace hpactor::metrics
