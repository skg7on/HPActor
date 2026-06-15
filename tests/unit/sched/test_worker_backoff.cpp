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

#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/worker_thread.hpp>

#include <gtest/gtest.h>

using namespace hpactor::sched;

// ── BackoffCalibration defaults ──────────────────────────────────────

TEST(WorkerBackoffTest, CalibrationDefaultYieldIsEffective) {
    BackoffCalibration cal;
    EXPECT_FALSE(cal.yield_is_effective);
}

TEST(WorkerBackoffTest, CalibrationDefaultMinSleepIsSane) {
    BackoffCalibration cal;
    EXPECT_GE(cal.min_effective_sleep_ns, 1'000u);
    EXPECT_LE(cal.min_effective_sleep_ns, 10'000'000u);
}

TEST(WorkerBackoffTest, CalibrationDefaultPollingBudgetIsSane) {
    BackoffCalibration cal;
    EXPECT_GE(cal.polling_budget_ns, 1'000'000u);
    EXPECT_LE(cal.polling_budget_ns, 100'000'000u);
}

// ── Test calibration injection ────────────────────────────────────────

TEST(WorkerBackoffTest, TestCalibrationInjection) {
    BackoffCalibration cal;
    cal.yield_is_effective = true;
    cal.min_effective_sleep_ns = 123'456;
    cal.spin_threshold_ns = 20'000;
    cal.polling_budget_ns = 50'000'000;

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();

    const auto& used = worker.calibration();
    EXPECT_TRUE(used.yield_is_effective);
    EXPECT_EQ(used.min_effective_sleep_ns, 123'456u);
    EXPECT_EQ(used.spin_threshold_ns, 20'000u);
    EXPECT_EQ(used.polling_budget_ns, 50'000'000u);

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}

TEST(WorkerBackoffTest, TestCalibrationClearedAfterNull) {
    WorkerThread::set_test_calibration(nullptr);
    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();
    const auto& used = worker.calibration();
    EXPECT_GE(used.min_effective_sleep_ns, 1'000u);
    EXPECT_LE(used.polling_budget_ns, 100'000'000u);
    worker.stop();
}

// ── WorkerSnapshot new fields ─────────────────────────────────────────

TEST(WorkerBackoffTest, WorkerSnapshotHasNewFields) {
    WorkerSnapshot ws;
    EXPECT_FALSE(ws.calibration_yield_effective);
    EXPECT_EQ(ws.calibration_min_sleep_ns, 0u);
    EXPECT_EQ(ws.consecutive_empty_wakes, 0u);
}

// ── Calibration probe ─────────────────────────────────────────────────

TEST(WorkerBackoffTest, CalibrationProbeYieldIsEffective) {
    WorkerThread::set_test_calibration(nullptr);
    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();

    const auto& cal = worker.calibration();
    // Sanity: min_effective_sleep_ns should be in [1us, 10ms].
    EXPECT_GE(cal.min_effective_sleep_ns, 1'000u);
    EXPECT_LE(cal.min_effective_sleep_ns, 10'000'000u);

    // Sanity: polling_budget_ns should be >= 10ms and <= 100ms.
    EXPECT_GE(cal.polling_budget_ns, 10'000'000u);
    EXPECT_LE(cal.polling_budget_ns, 100'000'000u);

    // spin_threshold_ns is 0 when yield is not effective, 20'000 otherwise.
    if (!cal.yield_is_effective) {
        EXPECT_EQ(cal.spin_threshold_ns, 0u);
    } else {
        EXPECT_EQ(cal.spin_threshold_ns, 20'000u);
    }

    worker.stop();
}

// ── Time-based backoff ────────────────────────────────────────────────

TEST(WorkerBackoffTest, WorkerReachesCvModelWithInjectedCalibration) {
    BackoffCalibration cal;
    cal.yield_is_effective = false;
    cal.min_effective_sleep_ns = 1'000; // 1us (fast)
    cal.spin_threshold_ns = 0;          // no spin
    cal.polling_budget_ns = 5'000'000;  // 5ms polling budget

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();

    // Wait for the worker to escalate to CV model.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.diag_is_in_cv_model() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(worker.diag_is_in_cv_model());
    EXPECT_GT(worker.diag_cv_escalations(), 0u);
    EXPECT_GT(worker.diag_idle_iters(), 0u);

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}

TEST(WorkerBackoffTest, BackoffResetsWhenWorkFound) {
    BackoffCalibration cal;
    cal.yield_is_effective = false;
    cal.min_effective_sleep_ns = 1'000;
    cal.spin_threshold_ns = 0;
    cal.polling_budget_ns = 10'000'000; // 10ms

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);

    std::atomic<bool> work_processed{false};
    worker.set_work_processor(
        [&](const WorkItem&) { work_processed.store(true); });

    worker.start();

    // Let worker go idle and enter CV.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.diag_is_in_cv_model() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(worker.diag_is_in_cv_model());

    // Push work.
    WorkItem item;
    item.actor = hpactor::ActorId{0};
    worker.push(0, item);

    // Wait for work to be processed.
    auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!work_processed.load() && std::chrono::steady_clock::now() < deadline2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(work_processed.load());

    // After processing work, worker should be out of CV model.
    EXPECT_FALSE(worker.diag_is_in_cv_model());

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}
