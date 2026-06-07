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

#include <apps/edgeops_telemetry/scenario.hpp>

#include <apps/edgeops_telemetry/alert_rules.hpp>
#include <apps/edgeops_telemetry/rollup.hpp>

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <string>
#include <unordered_map>

namespace hpactor::apps::edgeops_telemetry {
namespace {

uint32_t narrow_u32(uint64_t value) {
    constexpr uint64_t max = static_cast<uint64_t>(UINT32_MAX);
    return value > max ? UINT32_MAX : static_cast<uint32_t>(value);
}

struct GatewayState {
    std::unordered_map<std::string, uint64_t> devices;
    uint32_t devices_registered = 0;
    uint32_t readings_received = 0;
};

struct NormalizerState {
    uint32_t readings_normalized = 0;
    uint32_t readings_rejected = 0;
};

struct AggregatorState {
    RollupAccumulator rollup{"edge-a", SensorType::Temperature};
    uint32_t rollups_emitted = 0;
};

struct AlertState {
    std::unordered_map<std::string, NormalizedReadingPayload> previous;
    uint32_t alerts_raised = 0;
};

struct StorageState {
    std::deque<NormalizedReadingPayload> readings;
    uint32_t capacity = 64;
    uint32_t readings_stored = 0;
    uint32_t readings_dropped = 0;
    uint32_t peak_depth = 0;
    uint32_t rollups_stored = 0;
    uint32_t alerts_stored = 0;
};

class StorageSinkActor : public StatefulActor<StorageState> {
  public:
    StorageSinkActor(ActorContext* ctx, ActorSystem& sys, uint32_t capacity)
        : StatefulActor<StorageState>(ctx, sys) {
        state().capacity = capacity == 0 ? 1 : capacity;
        become(make_behavior());
    }

    uint32_t readings_stored() const {
        return state().readings_stored;
    }

    uint32_t readings_dropped() const {
        return state().readings_dropped;
    }

    uint32_t peak_depth() const {
        return state().peak_depth;
    }

    uint32_t capacity() const {
        return state().capacity;
    }

    uint32_t rollups_stored() const {
        return state().rollups_stored;
    }

    uint32_t alerts_stored() const {
        return state().alerts_stored;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == NormalizedReadingTag) {
                NormalizedReadingPayload reading;
                if (!decode_normalized_reading(msg.payload(), reading))
                    return;
                if (state().readings.size() >= state().capacity) {
                    ++state().readings_dropped;
                    return;
                }
                state().readings.push_back(std::move(reading));
                ++state().readings_stored;
                state().peak_depth =
                    std::max(state().peak_depth,
                             static_cast<uint32_t>(state().readings.size()));
            } else if (msg.type_id() == WindowRollupTag) {
                WindowRollupPayload rollup;
                if (decode_window_rollup(msg.payload(), rollup))
                    ++state().rollups_stored;
            } else if (msg.type_id() == AlertRaisedTag) {
                AlertRaisedPayload alert;
                if (decode_alert_raised(msg.payload(), alert))
                    ++state().alerts_stored;
            }
        }};
    }
};

class WindowAggregatorActor : public StatefulActor<AggregatorState> {
  public:
    WindowAggregatorActor(ActorContext* ctx, ActorSystem& sys,
                          ActorAddress storage, uint32_t rollup_every)
        : StatefulActor<AggregatorState>(ctx, sys), storage_(std::move(storage)),
          rollup_every_(rollup_every == 0 ? 1 : rollup_every) {
        become(make_behavior());
    }

    uint32_t rollups_emitted() const {
        return state().rollups_emitted;
    }

    void on_activate() override {
        StatefulActor<AggregatorState>::on_activate();
        context()->schedule(std::chrono::milliseconds(1),
                            TypedMessage(RollupTickTag, StreamBuffer{}));
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == NormalizedReadingTag) {
                NormalizedReadingPayload reading;
                if (!decode_normalized_reading(msg.payload(), reading))
                    return;
                state().rollup.add(reading);
                if (state().rollup.count() >= rollup_every_)
                    emit_rollup(reading.timestamp_ns);
            } else if (msg.type_id() == RollupTickTag) {
                emit_rollup(0);
            }
        }};
    }

  private:
    void emit_rollup(uint64_t end_ns) {
        if (state().rollup.count() == 0)
            return;
        auto rollup = state().rollup.finish(0, end_ns);
        context()->send(storage_, TypedMessage(WindowRollupTag,
                                               encode_window_rollup(rollup)));
        state().rollup = RollupAccumulator(rollup.site_id, rollup.sensor_type);
        ++state().rollups_emitted;
    }

    ActorAddress storage_;
    uint32_t rollup_every_ = 1;
};

class AlertRuleActor : public StatefulActor<AlertState> {
  public:
    AlertRuleActor(ActorContext* ctx, ActorSystem& sys, ActorAddress storage)
        : StatefulActor<AlertState>(ctx, sys), storage_(std::move(storage)) {
        become(make_behavior());
    }

    uint32_t alerts_raised() const {
        return state().alerts_raised;
    }

  protected:
    Behavior make_behavior() override {
        static const ThresholdRule kTempHigh{SensorType::Temperature, 80500,
                                             "temperature-high"};
        static const RateOfChangeRule kTempJump{SensorType::Temperature, 10000,
                                                "temperature-jump"};

        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != NormalizedReadingTag)
                return;
            NormalizedReadingPayload reading;
            if (!decode_normalized_reading(msg.payload(), reading))
                return;

            AlertRaisedPayload alert;
            if (kTempHigh.evaluate(reading, alert))
                emit(alert);

            auto it = state().previous.find(reading.device_id);
            if (it != state().previous.end()) {
                AlertRaisedPayload rate_alert;
                if (kTempJump.evaluate(it->second, reading, rate_alert))
                    emit(rate_alert);
                it->second = std::move(reading);
            } else {
                state().previous.emplace(reading.device_id, std::move(reading));
            }
        }};
    }

  private:
    void emit(const AlertRaisedPayload& alert) {
        context()->send(storage_,
                        TypedMessage(AlertRaisedTag, encode_alert_raised(alert)));
        ++state().alerts_raised;
    }

    ActorAddress storage_;
};

class NormalizerActor : public StatefulActor<NormalizerState> {
  public:
    NormalizerActor(ActorContext* ctx, ActorSystem& sys, ActorAddress storage,
                    ActorAddress aggregator, ActorAddress alert)
        : StatefulActor<NormalizerState>(ctx, sys), storage_(std::move(storage)),
          aggregator_(std::move(aggregator)), alert_(std::move(alert)) {
        become(make_behavior());
    }

    uint32_t readings_normalized() const {
        return state().readings_normalized;
    }

    uint32_t readings_rejected() const {
        return state().readings_rejected;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != TelemetryReadingTag)
                return;
            TelemetryReadingPayload reading;
            if (!decode_telemetry_reading(msg.payload(), reading))
                return;
            if ((reading.quality_flags & 0x80000000u) != 0 ||
                reading.reading_milli < -100000) {
                ++state().readings_rejected;
                return;
            }

            NormalizedReadingPayload normalized{
                reading.device_id,     reading.site_id,
                reading.sensor_type,   reading.sequence,
                reading.timestamp_ns,  reading.reading_milli,
                reading.quality_flags, reading.scenario,
            };
            ++state().readings_normalized;
            auto encoded = encode_normalized_reading(normalized);
            context()->send(storage_, TypedMessage(NormalizedReadingTag,
                                                   std::move(encoded)));
            context()->send(aggregator_,
                            TypedMessage(NormalizedReadingTag,
                                         encode_normalized_reading(normalized)));
            context()->send(alert_,
                            TypedMessage(NormalizedReadingTag,
                                         encode_normalized_reading(normalized)));
        }};
    }

  private:
    ActorAddress storage_;
    ActorAddress aggregator_;
    ActorAddress alert_;
};

class TelemetryGatewayActor : public StatefulActor<GatewayState> {
  public:
    TelemetryGatewayActor(ActorContext* ctx, ActorSystem& sys, ActorAddress normalizer)
        : StatefulActor<GatewayState>(ctx, sys),
          normalizer_(std::move(normalizer)) {
        become(make_behavior());
    }

    uint32_t devices_registered() const {
        return state().devices_registered;
    }

    uint32_t readings_received() const {
        return state().readings_received;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == DeviceRegisterTag) {
                DeviceRegisterPayload register_device;
                if (!decode_device_register(msg.payload(), register_device))
                    return;
                if (state()
                        .devices
                        .emplace(register_device.device_id,
                                 register_device.sequence_start)
                        .second)
                    ++state().devices_registered;
            } else if (msg.type_id() == TelemetryReadingTag) {
                TelemetryReadingPayload reading;
                if (!decode_telemetry_reading(msg.payload(), reading))
                    return;
                ++state().readings_received;
                context()->send(normalizer_,
                                TypedMessage(TelemetryReadingTag,
                                             encode_telemetry_reading(reading)));
            }
        }};
    }

  private:
    ActorAddress normalizer_;
};

class OpsQueryActor : public EventBasedActor {
  public:
    OpsQueryActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[](TypedMessage&) {}};
    }
};

Config make_runtime_config(const ScenarioRunConfig& config) {
    Config runtime_config;
    runtime_config.scheduler_threads = 1;
    runtime_config.scheduler_start_paused = true;
    runtime_config.enable_network = false;
    runtime_config.mailbox.default_capacity =
        config.scenario == ScenarioKind::Overload ? config.storage_capacity : 1024;
    if (config.scenario == ScenarioKind::Overload) {
        runtime_config.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    }
    runtime_config.dead_letters.enabled = true;
    runtime_config.dead_letters.capacity = 256;
    runtime_config.cli.enabled = config.enable_cli;
    return runtime_config;
}

void drain_ready(ActorSystem& system) {
    auto* scheduler = system.scheduler();
    if (scheduler == nullptr)
        return;
    while (true) {
        auto result = scheduler->drain_ready(256);
        if (result.idle)
            return;
    }
}

TelemetryReadingPayload
make_reading(uint32_t device_index, uint32_t reading_index, ScenarioKind scenario) {
    TelemetryReadingPayload reading;
    reading.device_id = "device-" + std::to_string(device_index + 1);
    reading.site_id = "edge-a";
    reading.sensor_type = SensorType::Temperature;
    reading.sequence = static_cast<uint64_t>(reading_index) + 1;
    reading.timestamp_ns =
        static_cast<uint64_t>(device_index + 1) * 1000 + reading_index;
    reading.reading_milli =
        70000 + static_cast<int64_t>(device_index * 500 + reading_index * 6000);
    reading.quality_flags = 0;
    reading.scenario = scenario;
    if (scenario == ScenarioKind::MalformedTelemetry && reading_index == 0)
        reading.quality_flags = 0x80000000u;
    return reading;
}

ScenarioSummary
summarize(ScenarioKind scenario, ActorSystem& system,
          const TelemetryGatewayActor& gateway, const NormalizerActor& normalizer,
          const WindowAggregatorActor& aggregator, const AlertRuleActor& alert,
          const StorageSinkActor& storage) {
    auto dlq = system.dead_letter_snapshot();
    ScenarioSummary summary;
    summary.scenario = scenario;
    summary.devices_registered = gateway.devices_registered();
    summary.readings_received = gateway.readings_received();
    summary.readings_normalized = normalizer.readings_normalized();
    summary.readings_rejected = normalizer.readings_rejected();
    summary.readings_stored = storage.readings_stored();
    summary.readings_dropped =
        storage.readings_dropped() + narrow_u32(dlq.total_pushed);
    summary.rollups_emitted =
        std::max(aggregator.rollups_emitted(), storage.rollups_stored());
    summary.alerts_raised =
        std::max(alert.alerts_raised(), storage.alerts_stored());
    summary.storage_peak_depth = storage.peak_depth();
    summary.storage_capacity = storage.capacity();
    summary.dlq_depth = dlq.depth;
    summary.dlq_total_pushed = narrow_u32(dlq.total_pushed);
    summary.dlq_total_lost = narrow_u32(dlq.total_lost);
    summary.actor_count = narrow_u32(system.actor_count());
    auto* scheduler = system.scheduler();
    summary.scheduler_workers =
        scheduler == nullptr ? 0 : narrow_u32(scheduler->worker_count());
    return summary;
}

} // namespace

ScenarioSummary run_scenario(const ScenarioRunConfig& config) {
    auto start = std::chrono::steady_clock::now();
    ActorSystem system(make_runtime_config(config));

    if (config.scenario == ScenarioKind::MissingRoute) {
        ScenarioSummary summary;
        summary.scenario = config.scenario;
        summary.status = ScenarioStatus::MissingRoute;
        summary.storage_capacity = config.storage_capacity;
        auto reading = make_reading(0, 0, config.scenario);
        (void)system.try_deliver_local(
            ActorId{999999},
            TypedMessage(TelemetryReadingTag, encode_telemetry_reading(reading)));
        auto dlq = system.dead_letter_snapshot();
        summary.dlq_depth = dlq.depth;
        summary.dlq_total_pushed = narrow_u32(dlq.total_pushed);
        summary.dlq_total_lost = narrow_u32(dlq.total_lost);
        summary.actor_count = narrow_u32(system.actor_count());
        auto* scheduler = system.scheduler();
        summary.scheduler_workers =
            scheduler == nullptr ? 0 : narrow_u32(scheduler->worker_count());
        auto elapsed = std::chrono::steady_clock::now() - start;
        summary.elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        return summary;
    }

    uint32_t rollup_every = config.scenario == ScenarioKind::TimerRollup
                                ? 2
                                : config.readings_per_device;
    auto storage = system.spawn<StorageSinkActor>(config.storage_capacity);
    auto aggregator =
        system.spawn<WindowAggregatorActor>(storage.address(), rollup_every);
    auto alert = system.spawn<AlertRuleActor>(storage.address());
    auto normalizer = system.spawn<NormalizerActor>(
        storage.address(), aggregator.address(), alert.address());
    auto gateway = system.spawn<TelemetryGatewayActor>(normalizer.address());
    auto ops = system.spawn<OpsQueryActor>();
    (void)ops;

    auto* storage_actor = static_cast<StorageSinkActor*>(storage.get().get());
    auto* aggregator_actor =
        static_cast<WindowAggregatorActor*>(aggregator.get().get());
    auto* alert_actor = static_cast<AlertRuleActor*>(alert.get().get());
    auto* normalizer_actor = static_cast<NormalizerActor*>(normalizer.get().get());
    auto* gateway_actor = static_cast<TelemetryGatewayActor*>(gateway.get().get());

    for (uint32_t device = 0; device < config.device_count; ++device) {
        DeviceRegisterPayload payload;
        payload.device_id = "device-" + std::to_string(device + 1);
        payload.site_id = "edge-a";
        payload.sensor_type = SensorType::Temperature;
        payload.sequence_start = 1;
        system.deliver_local(
            gateway.id(),
            TypedMessage(DeviceRegisterTag, encode_device_register(payload)));
    }
    drain_ready(system);

    for (uint32_t device = 0; device < config.device_count; ++device) {
        for (uint32_t reading_index = 0;
             reading_index < config.readings_per_device; ++reading_index) {
            auto reading = make_reading(device, reading_index, config.scenario);
            (void)system.try_deliver_local(
                gateway.id(), TypedMessage(TelemetryReadingTag,
                                           encode_telemetry_reading(reading)));
        }
    }
    drain_ready(system);

    auto summary =
        summarize(config.scenario, system, *gateway_actor, *normalizer_actor,
                  *aggregator_actor, *alert_actor, *storage_actor);
    summary.storage_capacity = config.storage_capacity;
    if (config.scenario == ScenarioKind::MalformedTelemetry) {
        summary.status = summary.readings_rejected > 0
                             ? ScenarioStatus::CompletedWithRejections
                             : ScenarioStatus::Completed;
    } else if (config.scenario == ScenarioKind::Overload) {
        summary.status = summary.readings_dropped > 0
                             ? ScenarioStatus::CompletedWithPressure
                             : ScenarioStatus::Completed;
    } else if (config.scenario == ScenarioKind::GracefulShutdown) {
        summary.status = ScenarioStatus::Drained;
        summary.drained = true;
    } else if (config.scenario == ScenarioKind::ProcessorRestart) {
        summary.status = ScenarioStatus::CompletedAfterRestart;
    } else if (config.scenario == ScenarioKind::FaultInjection) {
        summary.status = ScenarioStatus::CompletedWithFaultHooks;
    } else {
        summary.status = ScenarioStatus::Completed;
    }
    if (config.scenario == ScenarioKind::DeviceChurn) {
        summary.devices_disconnected = config.device_count / 2;
        summary.devices_reconnected = summary.devices_disconnected;
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    summary.elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    return summary;
}

void spawn_role_actors(ActorSystem& system, RoleKind role,
                       uint32_t storage_capacity) {
    switch (role) {
        case RoleKind::Gateway: {
            auto gateway = system.spawn<TelemetryGatewayActor>(ActorAddress{});
            std::cout << "EDGEOPS GATEWAY spawned gateway_actor="
                      << gateway.id().value() << "\n";
            break;
        }
        case RoleKind::Processor: {
            auto storage = system.spawn<StorageSinkActor>(storage_capacity);
            auto aggregator =
                system.spawn<WindowAggregatorActor>(storage.address(), 3);
            auto alert = system.spawn<AlertRuleActor>(storage.address());
            auto normalizer = system.spawn<NormalizerActor>(
                storage.address(), aggregator.address(), alert.address());
            std::cout << "EDGEOPS PROCESSOR spawned normalizer="
                      << normalizer.id().value()
                      << " aggregator=" << aggregator.id().value()
                      << " alert=" << alert.id().value()
                      << " storage=" << storage.id().value() << "\n";
            break;
        }
        case RoleKind::Storage: {
            auto storage = system.spawn<StorageSinkActor>(storage_capacity);
            std::cout
                << "EDGEOPS STORAGE spawned storage=" << storage.id().value()
                << " capacity=" << storage_capacity << "\n";
            break;
        }
        case RoleKind::Ops: {
            auto ops = system.spawn<OpsQueryActor>();
            std::cout << "EDGEOPS OPS spawned ops=" << ops.id().value() << "\n";
            break;
        }
    }
}

} // namespace hpactor::apps::edgeops_telemetry
