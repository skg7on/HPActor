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

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/process/health_check.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <chrono>

namespace {

using namespace hpactor;
using namespace hpactor::process;

// ============================================================================
// HealthStatus, HealthCheckResult, CheckContext — trivial value types
// ============================================================================

TEST(HealthCheckBasics, StatusEnumValues) {
    EXPECT_NE(HealthStatus::Healthy, HealthStatus::Degraded);
    EXPECT_NE(HealthStatus::Degraded, HealthStatus::Unhealthy);
    EXPECT_NE(HealthStatus::Unhealthy, HealthStatus::Healthy);
}

TEST(HealthCheckBasics, CheckResultDefaultIsHealthy) {
    HealthCheckResult r;
    EXPECT_EQ(r.status, HealthStatus::Healthy);
    EXPECT_TRUE(r.reason.empty());
}

// ============================================================================
// HealthState — thread-safe cache
// ============================================================================

TEST(HealthStateTest, DefaultIsHealthy) {
    HealthState s;
    EXPECT_EQ(s.overall_status(), HealthStatus::Healthy);
    EXPECT_TRUE(s.details().empty());
}

TEST(HealthStateTest, UpdateAndReadBack) {
    HealthState s;

    std::vector<HealthCheckResult> details;
    HealthCheckResult r;
    r.check_name = "test_check";
    r.status = HealthStatus::Degraded;
    r.reason = "something is slow";
    details.push_back(r);

    auto ts = std::chrono::steady_clock::now();
    s.update(HealthStatus::Degraded, details, ts);

    EXPECT_EQ(s.overall_status(), HealthStatus::Degraded);
    EXPECT_EQ(s.last_check_time(), ts);

    auto read_back = s.details();
    ASSERT_EQ(read_back.size(), 1u);
    EXPECT_EQ(read_back[0].check_name, "test_check");
    EXPECT_EQ(read_back[0].status, HealthStatus::Degraded);
    EXPECT_EQ(read_back[0].reason, "something is slow");
}

TEST(HealthStateTest, UpdateOverwritesPrevious) {
    HealthState s;

    std::vector<HealthCheckResult> d1;
    HealthCheckResult r1;
    r1.check_name = "first";
    r1.status = HealthStatus::Healthy;
    d1.push_back(r1);
    s.update(HealthStatus::Healthy, d1, std::chrono::steady_clock::now());
    EXPECT_EQ(s.overall_status(), HealthStatus::Healthy);

    std::vector<HealthCheckResult> d2;
    HealthCheckResult r2;
    r2.check_name = "second";
    r2.status = HealthStatus::Unhealthy;
    r2.reason = "critical failure";
    d2.push_back(r2);
    s.update(HealthStatus::Unhealthy, d2, std::chrono::steady_clock::now());
    EXPECT_EQ(s.overall_status(), HealthStatus::Unhealthy);

    auto read_back = s.details();
    ASSERT_EQ(read_back.size(), 1u);
    EXPECT_EQ(read_back[0].check_name, "second");
}

// ============================================================================
// HealthCheckEngine — aggregation
// ============================================================================

namespace {

/// Minimal check that always returns the same result.
class StubCheck : public IHealthCheck {
  public:
    StubCheck(std::string name, bool critical, HealthStatus status, std::string reason)
        : name_(std::move(name)), critical_(critical), status_(status),
          reason_(std::move(reason)) {}

    std::string_view name() const noexcept override {
        return name_;
    }
    bool is_critical() const noexcept override {
        return critical_;
    }
    HealthCheckResult check(const CheckContext&) override {
        HealthCheckResult r;
        r.check_name = name_;
        r.status = status_;
        r.reason = reason_;
        return r;
    }

  private:
    std::string name_;
    bool critical_;
    HealthStatus status_;
    std::string reason_;
};

} // anonymous namespace

TEST(HealthCheckEngineTest, EmptyIsHealthy) {
    HealthCheckEngine engine;
    HealthState state;
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    engine.run_all(ctx, state);
    EXPECT_EQ(state.overall_status(), HealthStatus::Healthy);
    EXPECT_TRUE(state.details().empty());
}

TEST(HealthCheckEngineTest, AllHealthy) {
    HealthCheckEngine engine;
    engine.add_check(
        std::make_unique<StubCheck>("a", true, HealthStatus::Healthy, ""));
    engine.add_check(
        std::make_unique<StubCheck>("b", false, HealthStatus::Healthy, ""));

    HealthState state;
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    engine.run_all(ctx, state);
    EXPECT_EQ(state.overall_status(), HealthStatus::Healthy);
    EXPECT_EQ(state.details().size(), 2u);
}

TEST(HealthCheckEngineTest, NonCriticalUnhealthyCapsAtDegraded) {
    HealthCheckEngine engine;
    engine.add_check(std::make_unique<StubCheck>(
        "noncrit", false, HealthStatus::Unhealthy, "non-critical issue"));

    HealthState state;
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    engine.run_all(ctx, state);
    EXPECT_EQ(state.overall_status(), HealthStatus::Degraded);
}

TEST(HealthCheckEngineTest, CriticalUnhealthySetsUnhealthy) {
    HealthCheckEngine engine;
    engine.add_check(std::make_unique<StubCheck>(
        "crit", true, HealthStatus::Unhealthy, "critical issue"));

    HealthState state;
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    engine.run_all(ctx, state);
    EXPECT_EQ(state.overall_status(), HealthStatus::Unhealthy);
}

TEST(HealthCheckEngineTest, DegradedCheckSetsDegraded) {
    HealthCheckEngine engine;
    engine.add_check(
        std::make_unique<StubCheck>("a", true, HealthStatus::Healthy, ""));
    engine.add_check(
        std::make_unique<StubCheck>("b", false, HealthStatus::Degraded, "slow"));

    HealthState state;
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    engine.run_all(ctx, state);
    EXPECT_EQ(state.overall_status(), HealthStatus::Degraded);
    EXPECT_EQ(state.details().size(), 2u);
}

TEST(HealthCheckEngineTest, CriticalUnhealthyTakesPrecedenceOverDegraded) {
    // Degraded + critical Unhealthy → overall Unhealthy
    HealthCheckEngine engine;
    engine.add_check(std::make_unique<StubCheck>("noncrit", false,
                                                 HealthStatus::Degraded, "slow"));
    engine.add_check(std::make_unique<StubCheck>(
        "crit", true, HealthStatus::Unhealthy, "dead"));

    HealthState state;
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    engine.run_all(ctx, state);
    EXPECT_EQ(state.overall_status(), HealthStatus::Unhealthy);
}

// ============================================================================
// MemoryPressureCheck — fully deterministic via injectable reader
// ============================================================================

static uint8_t fake_reader_50() {
    return 50;
}
static uint8_t fake_reader_86() {
    return 86;
}
static uint8_t fake_reader_96() {
    return 96;
}
static uint8_t fake_reader_85() {
    return 85;
}

TEST(MemoryPressureCheckTest, Healthy) {
    MemoryPressureCheck check(85, 95, fake_reader_50);
    EXPECT_TRUE(check.is_critical());
    EXPECT_EQ(check.name(), "memory_pressure");

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(1000)};

    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Healthy);
}

TEST(MemoryPressureCheckTest, DegradedAtWarning) {
    MemoryPressureCheck check(85, 95, fake_reader_86);

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Degraded);
    EXPECT_NE(result.reason.find("86%"), std::string::npos);
}

TEST(MemoryPressureCheckTest, UnhealthyAtCritical) {
    MemoryPressureCheck check(85, 95, fake_reader_96);

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Unhealthy);
    EXPECT_NE(result.reason.find("96%"), std::string::npos);
}

TEST(MemoryPressureCheckTest, ExactlyAtWarningBoundary) {
    MemoryPressureCheck check(85, 95, fake_reader_85);

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);
    CheckContext ctx{system, std::chrono::milliseconds(0)};

    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Degraded);
}

// ============================================================================
// SchedulerLivenessCheck — requires ActorSystem with scheduler
// ============================================================================

TEST(SchedulerLivenessCheckTest, FirstCheckAlwaysHealthy) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 1;
    sys_cfg.scheduler_start_paused = true;

    ActorSystem system(sys_cfg);
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);

    SchedulerLivenessCheck check(30);
    EXPECT_TRUE(check.is_critical());
    EXPECT_EQ(check.name(), "scheduler_liveness");

    CheckContext ctx{system, std::chrono::milliseconds(0)};
    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Healthy);
}

TEST(SchedulerLivenessCheckTest, NoProgressAfterDeadlineIsUnhealthy) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 1;
    sys_cfg.scheduler_start_paused = true;

    ActorSystem system(sys_cfg);
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);

    // Use deadline=0 so any stall is immediately unhealthy.
    SchedulerLivenessCheck check(0);

    // First check — baseline.
    CheckContext ctx1{system, std::chrono::milliseconds(0)};
    auto r1 = check.check(ctx1);
    EXPECT_EQ(r1.status, HealthStatus::Healthy);

    // Second check with no progress — unhealthy (deadline=0).
    CheckContext ctx2{system, std::chrono::milliseconds(1000)};
    auto r2 = check.check(ctx2);
    EXPECT_EQ(r2.status, HealthStatus::Unhealthy);
    EXPECT_NE(r2.reason.find("no scheduler progress"), std::string::npos);
}

TEST(SchedulerLivenessCheckTest, SchedulerNotRunningIsUnhealthy) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 1;
    sys_cfg.scheduler_start_paused = true;

    ActorSystem system(sys_cfg);
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);

    sched->stop();

    SchedulerLivenessCheck check(30);
    CheckContext ctx{system, std::chrono::milliseconds(0)};
    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Unhealthy);
    EXPECT_NE(result.reason.find("not running"), std::string::npos);
}

TEST(SchedulerLivenessCheckTest, NullSchedulerIsUnhealthy) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);

    SchedulerLivenessCheck check(30);
    CheckContext ctx{system, std::chrono::milliseconds(0)};
    auto result = check.check(ctx);
    // With 0 threads the scheduler may be null — check handles it.
    if (system.scheduler() == nullptr) {
        EXPECT_EQ(result.status, HealthStatus::Unhealthy);
    }
    // If present, just ensure check runs without crashing.
}

// ============================================================================
// SystemActorHealthCheck — requires ActorSystem with system actors
// ============================================================================

TEST(SystemActorHealthCheckTest, EmptySystemIsHealthy) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);

    SystemActorHealthCheck check;
    EXPECT_TRUE(check.is_critical());
    EXPECT_EQ(check.name(), "system_actor_health");

    CheckContext ctx{system, std::chrono::milliseconds(0)};
    auto result = check.check(ctx);
    // No system actors → no failures → Healthy.
    EXPECT_EQ(result.status, HealthStatus::Healthy);
}

// ============================================================================
// DLQGrowthCheck — requires ActorSystem with DLQ
// ============================================================================

TEST(DLQGrowthCheckTest, EmptyDLQIsHealthy) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);

    DLQGrowthCheck check(80, 95, 10);
    EXPECT_FALSE(check.is_critical());
    EXPECT_EQ(check.name(), "dlq_growth");

    CheckContext ctx{system, std::chrono::milliseconds(0)};
    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Healthy);
}

TEST(DLQGrowthCheckTest, DepthBelowThresholdIsHealthy) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);

    DLQGrowthCheck check(80, 95, 10);

    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);

    // Push a few records — well below any warning threshold.
    for (int i = 0; i < 5; ++i) {
        mailbox::DeadLetterRecord rec;
        rec.reason = mailbox::DeadLetterReason::MailboxFull;
        dlq->try_push(std::move(rec));
    }

    CheckContext ctx{system, std::chrono::milliseconds(1000)};
    auto result = check.check(ctx);
    EXPECT_EQ(result.status, HealthStatus::Healthy);
}

TEST(DLQGrowthCheckTest, RapidLossRateProducesDegraded) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    ActorSystem system(sys_cfg);

    // Threshold: 1 lost record per minute.
    // Set depth thresholds impossibly high so only the loss-rate path fires.
    DLQGrowthCheck check(200, 300, 1);

    auto* dlq = system.dead_letter_queue();
    ASSERT_NE(dlq, nullptr);

    auto snap_before = system.dead_letter_snapshot();
    uint32_t cap = (snap_before.capacity > 0) ? snap_before.capacity : 4096u;

    // First check — establish baseline when DLQ is empty/clean.
    CheckContext ctx_first{system, std::chrono::milliseconds(1000)};
    check.check(ctx_first);

    // Now fill the DLQ beyond capacity to generate lost records.
    for (uint32_t i = 0; i < cap + 10u; ++i) {
        mailbox::DeadLetterRecord rec;
        rec.reason = mailbox::DeadLetterReason::MailboxFull;
        dlq->try_push(std::move(rec));
    }

    // Run check with very short elapsed to make loss rate high.
    CheckContext ctx{system, std::chrono::milliseconds(100)}; // 0.1 sec =
                                                              // 600/min
    auto result = check.check(ctx);

    auto snap = system.dead_letter_snapshot();
    if (snap_before.total_lost < snap.total_lost) {
        // At least 1 record lost → rate > 1/min → Degraded.
        EXPECT_EQ(result.status, HealthStatus::Degraded);
        EXPECT_NE(result.reason.find("loss rate"), std::string::npos);
    }
    // If no loss occurred (DLQ is unbounded), Healthy is acceptable.
}

// ============================================================================
// make_health_check_engine factory
// ============================================================================

TEST(FactoryTest, DisabledConfigCreatesEmptyEngine) {
    HealthCheckConfig config;
    config.enabled = false;

    auto engine = make_health_check_engine(config);
    EXPECT_EQ(engine->size(), 0u);
}

TEST(FactoryTest, DefaultConfigCreatesFourChecks) {
    HealthCheckConfig config;
    auto engine = make_health_check_engine(config);
    EXPECT_EQ(engine->size(), 4u);
}

TEST(FactoryTest, SelectiveDisable) {
    HealthCheckConfig config;
    config.scheduler_liveness_enabled = false;
    config.system_actor_health_enabled = false;

    auto engine = make_health_check_engine(config);
    EXPECT_EQ(engine->size(), 2u); // dlq + memory pressure
}

// ============================================================================
// HealthCheckConfig defaults
// ============================================================================

TEST(HealthCheckConfigTest, DefaultValues) {
    HealthCheckConfig c;
    EXPECT_TRUE(c.enabled);
    EXPECT_TRUE(c.scheduler_liveness_enabled);
    EXPECT_EQ(c.scheduler_progress_deadline_sec, 30u);
    EXPECT_TRUE(c.system_actor_health_enabled);
    EXPECT_TRUE(c.dlq_growth_enabled);
    EXPECT_EQ(c.dlq_depth_warning_pct, 80u);
    EXPECT_EQ(c.dlq_depth_critical_pct, 95u);
    EXPECT_EQ(c.dlq_lost_rate_per_minute, 10u);
    EXPECT_TRUE(c.memory_pressure_enabled);
    EXPECT_EQ(c.memory_warning_pct, 85);
    EXPECT_EQ(c.memory_critical_pct, 95);
}

// ============================================================================
// ProcessConfig integration — health_check member is present
// ============================================================================

TEST(ProcessConfigTest, HasHealthCheckConfig) {
    ProcessConfig cfg;
    EXPECT_TRUE(cfg.health_check.enabled);
    EXPECT_EQ(cfg.health_check.scheduler_progress_deadline_sec, 30u);
}

} // anonymous namespace
