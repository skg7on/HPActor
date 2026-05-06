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

// =============================================================================
// HPActor Example 11: CLI Interactive Demo
// =============================================================================
//
// A complex actor system demonstrating CLI interactive introspection:
//
//   12 actors across 7 types:
//     - 4 × WorkerActor       — periodic task processing (every 100ms)
//     - 1 × AggregatorActor   — collects results, running stats
//     - 1 × HealthCheckActor  — pings workers (every 500ms)
//     - 1 × BroadcastActor    — config broadcasts (every 1s)
//     - 1 × ClockActor        — logical clock, time queries
//     - 1 × LogActor          — ring-buffer event log
//     - 1 × SystemMonitorActor — system-wide stats (every 2s)
//     - 1 × CliActor          — interactive CLI (DaemonActor, stdin)
//
//   Scheduler: 4 worker threads with A2WS work-stealing
//   CLI:       enabled, opt-in via CliConfig
//
//   CLI commands:
//     /actor <id> show  — InspectStateRequest → target actor → reply
//     /actor <id> kill  — KillRequest → target actor → terminate
//     /actor list       — enumerate all actors via registry
//     /system stats     — system-wide statistics
//     /system memory    — memory subsystem status
//     /help, /quit
//
//   Every actor overrides to_metadata() and serialize_state() so the CLI
//   can inspect internal state without ever reading actor memory directly.
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// Message Type Tags — application range (0x00010000 – 0x000100FF)
// =============================================================================

static const hpactor::TypeTag WorkerResultTag{0x00010000};
static const hpactor::TypeTag HealthPingTag{0x00010001};
static const hpactor::TypeTag HealthPongTag{0x00010002};
static const hpactor::TypeTag BroadcastConfigTag{0x00010003};
static const hpactor::TypeTag TimeQueryTag{0x00010004};
static const hpactor::TypeTag TimeReplyTag{0x00010005};
static const hpactor::TypeTag LogEntryTag{0x00010006};
static const hpactor::TypeTag MonitorQueryTag{0x00010007};
static const hpactor::TypeTag MonitorReplyTag{0x00010008};
static const hpactor::TypeTag PeriodicTickTag{0x00010009};
static const hpactor::TypeTag StartTag{0x0001000A};

// =============================================================================
// Payload helpers — simple int/double encode/decode via StreamBuffer
// =============================================================================

static hpactor::StreamBuffer encode_u64(uint64_t v) {
    hpactor::StreamBuffer buf(sizeof(v));
    std::memcpy(buf.data(), &v, sizeof(v));
    return buf;
}

static hpactor::TypedMessage make_msg(hpactor::TypeTag tag,
                                      hpactor::StreamBuffer payload = {}) {
    return hpactor::TypedMessage(tag, std::move(payload));
}

// =============================================================================
// Actor: ClockActor — maintains logical time, responds to time queries
// =============================================================================

class ClockActor : public hpactor::EventBasedActor {
public:
    ClockActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        epoch_start_ = std::chrono::steady_clock::now();
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "ClockActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "logical_time_us=" << logical_time_us_
            << " queries_answered=" << queries_answered_
            << " uptime_ms=" << elapsed_ms();
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == TimeQueryTag) {
                auto now = std::chrono::steady_clock::now();
                logical_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - epoch_start_).count();
                queries_answered_++;
                context()->reply(make_msg(TimeReplyTag,
                    encode_u64(static_cast<uint64_t>(logical_time_us_))));
            } else if (msg.type_id() == PeriodicTickTag) {
                logical_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - epoch_start_).count();
                context()->schedule(std::chrono::milliseconds(100),
                                    make_msg(PeriodicTickTag));
            }
        }};
    }

private:
    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_).count());
    }

    std::chrono::steady_clock::time_point epoch_start_;
    int64_t logical_time_us_ = 0;
    uint64_t queries_answered_ = 0;
    std::atomic<uint64_t> processed_{0};
};

// =============================================================================
// Actor: LogActor — ring-buffer event log (StatefulActor)
// =============================================================================

struct LogRingBuffer {
    static constexpr size_t kCapacity = 256;
    std::deque<std::string> entries;

    void append(const std::string& entry) {
        if (entries.size() >= kCapacity) entries.pop_front();
        entries.push_back(entry);
    }

    size_t size() const { return entries.size(); }
};

class LogActor : public hpactor::StatefulActor<LogRingBuffer> {
public:
    LogActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::StatefulActor<LogRingBuffer>(ctx, sys) {
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "LogActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        auto& buf = state();
        oss << "total_events=" << total_events_
            << " ring_depth=" << buf.size()
            << " capacity=" << LogRingBuffer::kCapacity;
        if (!buf.entries.empty()) {
            oss << "\n  last_5_events:";
            size_t start = buf.entries.size() > 5
                ? buf.entries.size() - 5 : 0;
            for (size_t i = start; i < buf.entries.size(); ++i) {
                oss << "\n    " << buf.entries[i];
            }
        }
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == LogEntryTag) {
                std::string entry(msg.payload().begin(), msg.payload().end());
                state().append(entry);
                total_events_++;
            }
        }};
    }

private:
    uint64_t elapsed_ms() const { return 0; }  // simplified
    uint64_t total_events_ = 0;
    std::atomic<uint64_t> processed_{0};
};

// =============================================================================
// Actor: WorkerActor — periodic task processing
// =============================================================================

class WorkerActor : public hpactor::EventBasedActor {
public:
    WorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                uint32_t worker_id, hpactor::ActorId aggregator_id,
                hpactor::ActorId log_id)
        : hpactor::EventBasedActor(ctx, sys)
        , worker_id_(worker_id)
        , aggregator_id_(aggregator_id)
        , log_id_(log_id) {
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "WorkerActor";
        m.state = healthy_ ? "Running" : "Unhealthy";
        m.messages_processed = processed_.load();
        m.uptime_ms = 0;
        std::ostringstream oss;
        oss << "worker-" << worker_id_;
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "worker_id=" << worker_id_
            << " tasks_processed=" << tasks_processed_
            << " avg_latency_us=" << std::fixed << std::setprecision(1)
            << avg_latency_us_
            << " current_load=" << std::fixed << std::setprecision(2)
            << current_load_
            << " healthy=" << (healthy_ ? "yes" : "no");
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                do_work();
            } else if (msg.type_id() == HealthPingTag) {
                // Reply with pong containing current load
                auto load = static_cast<uint64_t>(current_load_ * 1000.0);
                context()->reply(make_msg(HealthPongTag, encode_u64(load)));
            } else if (msg.type_id() == BroadcastConfigTag) {
                // Config broadcast received — in a real system, this would
                // update batch sizes, rate limits, etc.
            }
        }};
    }

private:
    void do_work() {
        // Simulate variable-latency work (50–500us)
        double latency = 50.0 + (rand() % 450);
        double throughput = 1000.0 + (rand() % 4000);

        tasks_processed_++;
        avg_latency_us_ = (avg_latency_us_ * 0.9) + (latency * 0.1);
        current_load_ = 0.3 + ((rand() % 70) / 100.0);

        // Send result to Aggregator
        hpactor::StreamBuffer payload(16);
        auto* data = reinterpret_cast<double*>(payload.data());
        data[0] = latency;
        data[1] = throughput;
        context()->send(aggregator_addr_, make_msg(WorkerResultTag,
                          std::move(payload)));

        // Log significant events
        if (tasks_processed_ % 100 == 0) {
            char buf[128];
            int n = snprintf(buf, sizeof(buf),
                "[Worker-%u] %llu tasks, avg %.0f us",
                worker_id_,
                static_cast<unsigned long long>(tasks_processed_),
                avg_latency_us_);
            hpactor::StreamBuffer log_payload(
                reinterpret_cast<const uint8_t*>(buf),
                reinterpret_cast<const uint8_t*>(buf + n));
            context()->send(log_addr_, make_msg(LogEntryTag,
                              std::move(log_payload)));
        }

        // Schedule next tick
        context()->schedule(std::chrono::milliseconds(100),
                            make_msg(PeriodicTickTag));
    }

    uint32_t worker_id_;
    hpactor::ActorId aggregator_id_;
    hpactor::ActorAddress aggregator_addr_;
    hpactor::ActorId log_id_;
    hpactor::ActorAddress log_addr_;
    uint64_t tasks_processed_ = 0;
    double avg_latency_us_ = 0.0;
    double current_load_ = 0.0;
    bool healthy_ = true;
    std::atomic<uint64_t> processed_{0};

public:
    void set_aggregator_addr(hpactor::ActorAddress a) { aggregator_addr_ = a; }
    void set_log_addr(hpactor::ActorAddress a) { log_addr_ = a; }
};

// =============================================================================
// Actor: AggregatorActor — collects worker results, maintains stats
// =============================================================================

class AggregatorActor : public hpactor::EventBasedActor {
public:
    AggregatorActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        latencies_.reserve(4096);
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "AggregatorActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = 0;
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "total_processed=" << total_processed_
            << " avg_latency_us=" << std::fixed << std::setprecision(1)
            << avg_latency_us_
            << " p50_us=" << std::fixed << std::setprecision(1) << p50_us_
            << " p99_us=" << std::fixed << std::setprecision(1) << p99_us_
            << " active_workers=" << active_workers_;
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == WorkerResultTag) {
                if (msg.payload().size() >= 16) {
                    auto* data = reinterpret_cast<const double*>(
                        msg.payload().data());
                    double latency = data[0];
                    // throughput = data[1] — available for rate calculations

                    total_processed_++;
                    avg_latency_us_ = (avg_latency_us_ * 0.95) + (latency * 0.05);
                    latencies_.push_back(latency);

                    // Recompute percentiles periodically
                    if (latencies_.size() >= 100) {
                        std::sort(latencies_.begin(), latencies_.end());
                        p50_us_ = latencies_[latencies_.size() / 2];
                        p99_us_ = latencies_[latencies_.size() * 99 / 100];
                        if (latencies_.size() > 2000) {
                            latencies_.erase(latencies_.begin(),
                                latencies_.begin() + 1000);
                        }
                    }
                }
            } else if (msg.type_id() == MonitorQueryTag) {
                // Reply with current aggregate stats
                std::array<uint64_t, 5> stats = {
                    total_processed_,
                    static_cast<uint64_t>(avg_latency_us_),
                    static_cast<uint64_t>(p50_us_),
                    static_cast<uint64_t>(p99_us_),
                    active_workers_
                };
                hpactor::StreamBuffer payload(
                    reinterpret_cast<const uint8_t*>(stats.data()),
                    reinterpret_cast<const uint8_t*>(stats.data() + 5));
                context()->reply(make_msg(MonitorReplyTag,
                                          std::move(payload)));
            }
        }};
    }

private:
    uint64_t total_processed_ = 0;
    double avg_latency_us_ = 0.0;
    double p50_us_ = 0.0;
    double p99_us_ = 0.0;
    uint32_t active_workers_ = 4;
    std::vector<double> latencies_;
    std::atomic<uint64_t> processed_{0};
};

// =============================================================================
// Actor: HealthCheckActor — periodically pings workers, tracks health
// =============================================================================

class HealthCheckActor : public hpactor::EventBasedActor {
public:
    HealthCheckActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    void add_worker(hpactor::ActorAddress addr) {
        workers_.push_back(addr);
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "HealthCheckActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = 0;
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "healthy_count=" << healthy_count_
            << " unhealthy_count=" << unhealthy_count_
            << " workers_tracked=" << workers_.size();
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                do_health_check();
            } else if (msg.type_id() == HealthPongTag) {
                healthy_count_++;
                unhealthy_count_ = static_cast<uint32_t>(workers_.size()) - healthy_count_;
            }
        }};
    }

private:
    void do_health_check() {
        healthy_count_ = 0;
        for (auto& addr : workers_) {
            context()->send(addr, make_msg(HealthPingTag));
        }
        context()->schedule(std::chrono::milliseconds(500),
                            make_msg(PeriodicTickTag));
    }

    std::vector<hpactor::ActorAddress> workers_;
    uint32_t healthy_count_ = 0;
    uint32_t unhealthy_count_ = 0;
    std::atomic<uint64_t> processed_{0};
};

// =============================================================================
// Actor: BroadcastActor — periodic config broadcasts to all workers
// =============================================================================

class BroadcastActor : public hpactor::EventBasedActor {
public:
    BroadcastActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    void add_worker(hpactor::ActorAddress addr) {
        workers_.push_back(addr);
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BroadcastActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = 0;
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "broadcasts_sent=" << broadcasts_sent_
            << " target_workers=" << workers_.size();
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                do_broadcast();
            }
        }};
    }

private:
    void do_broadcast() {
        char buf[64];
        int n = snprintf(buf, sizeof(buf),
            "config:v%llu:batch_size=%u",
            static_cast<unsigned long long>(broadcasts_sent_),
            static_cast<unsigned>(16 + (broadcasts_sent_ % 3) * 8));
        hpactor::StreamBuffer payload(
            reinterpret_cast<const uint8_t*>(buf),
            reinterpret_cast<const uint8_t*>(buf + n));
        for (auto& addr : workers_) {
            context()->send(addr, make_msg(BroadcastConfigTag, payload));
        }
        broadcasts_sent_++;
        context()->schedule(std::chrono::milliseconds(1000),
                            make_msg(PeriodicTickTag));
    }

    std::vector<hpactor::ActorAddress> workers_;
    uint64_t broadcasts_sent_ = 0;
    std::atomic<uint64_t> processed_{0};
};

// =============================================================================
// Actor: SystemMonitorActor — gathers system-wide stats
// =============================================================================

class SystemMonitorActor : public hpactor::EventBasedActor {
public:
    SystemMonitorActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "SystemMonitorActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = 0;
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "total_messages=" << total_messages_
            << " running_actors=" << running_actors_
            << " idle_actors=" << idle_actors_
            << " scheduler_utilization=" << std::fixed << std::setprecision(2)
            << scheduler_utilization_;
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                gather_stats();
            }
        }};
    }

private:
    void gather_stats() {
        total_messages_ = 0;
        running_actors_ = 0;
        idle_actors_ = 0;

        system().for_each_actor([&](hpactor::ActorId /*id*/,
                                     hpactor::AbstractActor& actor) {
            auto meta = actor.to_metadata();
            total_messages_ += meta.messages_processed;
            if (meta.state == "Running") running_actors_++;
            else if (meta.state == "Idle") idle_actors_++;
        });

        scheduler_utilization_ = running_actors_ > 0 ? 0.5 + (rand() % 40) / 100.0 : 0.0;

        context()->schedule(std::chrono::milliseconds(2000),
                            make_msg(PeriodicTickTag));
    }

    uint64_t total_messages_ = 0;
    uint32_t running_actors_ = 0;
    uint32_t idle_actors_ = 0;
    double scheduler_utilization_ = 0.0;
    std::atomic<uint64_t> processed_{0};
};

// =============================================================================
// Utility: deliver message to an actor from outside the actor system
// =============================================================================

static void send_to_actor(hpactor::ActorSystem& system,
                          hpactor::ActorId target, hpactor::TypeTag tag,
                          hpactor::StreamBuffer payload = {}) {
    system.deliver_local(target, hpactor::TypedMessage(tag, std::move(payload)));
}

// =============================================================================
// Utility: format ActorId as hex string
// =============================================================================

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 11: CLI Interactive Demo ===" << std::endl;
    std::cout << "\nArchitecture:" << std::endl;
    std::cout << "  12 actors: 4xWorker, Aggregator, HealthCheck, Broadcast,"
              << std::endl;
    std::cout << "             Clock, Log, SystemMonitor, CliActor"
              << std::endl;
    std::cout << "  4 scheduler threads with A2WS work-stealing" << std::endl;
    std::cout << "  CLI: Tab completion, hints, history, syntax highlighting"
              << std::endl;
    std::cout << std::endl;

    // Configure: 4 threads, CLI enabled
    hpactor::Config config;
    config.scheduler_threads = 4;
    config.max_queue_depth = 1024;
    config.cli = hpactor::cli::CliConfig{
        .enabled = true,
        .listen_path = "",
        .tcp_port = 0,
        .default_format = "pretty",
        .page_size = 20,
        .history_path = "",
        .history_max = 1000
    };

    // IMPORTANT: ActorSystem construction starts the CliActor daemon thread
    // which takes over stdin/stdout. All diagnostic output must happen BEFORE
    // this point, or be routed through the CLI formatter. We print setup info
    // first, then spawn actors silently.
    hpactor::ActorSystem system(config);

    // ── Spawn all actors (silently — CliActor owns stdout now) ─────────

    auto log_actor = system.spawn<LogActor>();
    auto clock = system.spawn<ClockActor>();
    auto aggregator = system.spawn<AggregatorActor>();
    auto monitor = system.spawn<SystemMonitorActor>();
    auto health_check = system.spawn<HealthCheckActor>();
    auto broadcast = system.spawn<BroadcastActor>();

    std::vector<std::shared_ptr<WorkerActor>> workers;
    for (uint32_t w = 0; w < 4; ++w) {
        auto worker = system.spawn<WorkerActor>(
            w + 1, aggregator.id(), log_actor.id());
        workers.push_back(std::static_pointer_cast<WorkerActor>(
            system.get_actor(worker.id())));
    }

    // ── Wire up addresses (set after spawn so addresses are known) ──

    for (auto& w : workers) {
        w->set_aggregator_addr(aggregator.address());
        w->set_log_addr(log_actor.address());
    }

    auto* health_raw = std::static_pointer_cast<HealthCheckActor>(
        system.get_actor(health_check.id())).get();
    auto* broadcast_raw = std::static_pointer_cast<BroadcastActor>(
        system.get_actor(broadcast.id())).get();

    for (auto& w : workers) {
        health_raw->add_worker(w->address());
        broadcast_raw->add_worker(w->address());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── Kick off periodic work ───────────────────────────────────────

    for (auto& w : workers) {
        send_to_actor(system, w->id(), StartTag);
    }
    send_to_actor(system, health_check.id(), StartTag);
    send_to_actor(system, broadcast.id(), StartTag);
    send_to_actor(system, monitor.id(), StartTag);
    send_to_actor(system, clock.id(), PeriodicTickTag);

    // Let the actors initialize before the user starts typing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Keep the main thread alive. The CliActor's DaemonActor thread
    // handles stdin. When the user types /quit or sends EOF, the CLI
    // loop exits and is_running() returns false.
    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n=== Demo Complete ===" << std::endl;
    return 0;
}
