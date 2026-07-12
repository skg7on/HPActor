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

#include <hpactor/config/python_binding_config.hpp>
#include <hpactor/python/python_bridge_types.hpp>
#include <hpactor/python/python_runtime.hpp>

using namespace hpactor;

TEST(PythonBindingConfigTest, DefaultsAreApproved) {
    config::PythonBindingConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.dispatch_queue_capacity, 65536u);
    EXPECT_EQ(cfg.command_queue_capacity, 16384u);
    EXPECT_EQ(cfg.completion_queue_capacity, 16384u);
    EXPECT_EQ(cfg.max_actor_bindings, 65536u);
    EXPECT_EQ(cfg.max_dispatch_per_tick, 256u);
    EXPECT_EQ(cfg.max_commands_per_turn, 256u);
    EXPECT_EQ(cfg.loop_lag_unready_ms, 5000u);
    EXPECT_EQ(cfg.handler_shutdown_timeout_ms, 10000u);
    EXPECT_EQ(cfg.topology_start_timeout_ms, 30000u);
    EXPECT_TRUE(cfg.trace_handler_spans);
}

TEST(PythonBindingConfigTest, ValidateAcceptsApprovedValues) {
    config::PythonBindingConfig cfg;
    cfg.enabled = true;
    cfg.dispatch_queue_capacity = 65536;
    cfg.command_queue_capacity = 16384;
    cfg.completion_queue_capacity = 16384;
    cfg.max_actor_bindings = 65536;
    cfg.max_dispatch_per_tick = 256;
    cfg.max_commands_per_turn = 256;
    cfg.loop_lag_unready_ms = 5000;
    cfg.handler_shutdown_timeout_ms = 10000;
    EXPECT_TRUE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsNonPowerOfTwoQueueCapacity) {
    config::PythonBindingConfig cfg;
    cfg.dispatch_queue_capacity = 65;
    EXPECT_FALSE(cfg.validate().ok());
    cfg = {};
    cfg.command_queue_capacity = 63;
    EXPECT_FALSE(cfg.validate().ok());
    cfg = {};
    cfg.completion_queue_capacity = 100;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsQueueCapacityBelowMin) {
    config::PythonBindingConfig cfg;
    cfg.dispatch_queue_capacity = 32;
    EXPECT_FALSE(cfg.validate().ok());
    cfg = {};
    cfg.dispatch_queue_capacity = 63;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsQueueCapacityAboveMax) {
    config::PythonBindingConfig cfg;
    cfg.dispatch_queue_capacity = 2 * 1024 * 1024; // 2097152, above 1048576
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsDrainBudgetBelowMin) {
    config::PythonBindingConfig cfg;
    cfg.max_dispatch_per_tick = 0;
    EXPECT_FALSE(cfg.validate().ok());
    cfg = {};
    cfg.max_commands_per_turn = 0;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsDrainBudgetAboveMax) {
    config::PythonBindingConfig cfg;
    cfg.max_dispatch_per_tick = 4097;
    EXPECT_FALSE(cfg.validate().ok());
    cfg = {};
    cfg.max_commands_per_turn = 4097;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsBudgetAboveCapacity) {
    config::PythonBindingConfig cfg;
    cfg.dispatch_queue_capacity = 64;
    cfg.max_dispatch_per_tick = 65;
    EXPECT_FALSE(cfg.validate().ok());
    cfg = {};
    cfg.command_queue_capacity = 64;
    cfg.max_commands_per_turn = 65;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsActorBindingsBelowMin) {
    config::PythonBindingConfig cfg;
    cfg.max_actor_bindings = 0;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsActorBindingsAboveMax) {
    config::PythonBindingConfig cfg;
    cfg.max_actor_bindings = 1'048'577;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsLoopLagBelowMin) {
    config::PythonBindingConfig cfg;
    cfg.loop_lag_unready_ms = 99;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsLoopLagAboveMax) {
    config::PythonBindingConfig cfg;
    cfg.loop_lag_unready_ms = 60'001;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsShutdownTimeoutBelowMin) {
    config::PythonBindingConfig cfg;
    cfg.handler_shutdown_timeout_ms = 99;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(PythonBindingConfigTest, RejectsShutdownTimeoutAboveMax) {
    config::PythonBindingConfig cfg;
    cfg.handler_shutdown_timeout_ms = 300'001;
    EXPECT_FALSE(cfg.validate().ok());
}

// ── Append-only contract tests ───────────────────────────────────────────
TEST(PythonPhase1CContractsTest, DispatchKindValuesAreAppendOnly) {
    EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::Message), 0u);
    EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::LinkedExit), 1u);
    EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::MonitorDown), 2u);
    EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::Restart), 3u);
}

TEST(PythonPhase1CContractsTest, CompletionKindValuesAreAppendOnly) {
    // Phase 1B existing values (CommandResult=0, AskResult=1, DeliveryResult=2,
    // SpawnResult=3, InspectResult=4, ScheduleResult=5, ActorStopped=6,
    // ActorFailed=7). Phase 1C does not renumber.
    EXPECT_EQ(static_cast<uint8_t>(python::PythonCompletionKind::InspectResult), 4u);
    EXPECT_EQ(static_cast<uint8_t>(python::PythonCompletionKind::ScheduleResult),
              5u);
    EXPECT_EQ(static_cast<uint8_t>(python::PythonCompletionKind::ActorStopped), 6u);
    EXPECT_EQ(static_cast<uint8_t>(python::PythonCompletionKind::ActorFailed), 7u);
}

TEST(PythonPhase1CContractsTest, FailureMetadataDefaultUsesLanguageBinding) {
    python::PythonFailureMetadata fm;
    EXPECT_EQ(fm.source, FailureSource::LanguageBinding);
    EXPECT_EQ(fm.reason, FailureReason::Unknown);
    EXPECT_EQ(fm.error_code, 0u);
    EXPECT_TRUE(fm.exception_type.empty());
    EXPECT_TRUE(fm.detail.empty());
    EXPECT_TRUE(fm.traceback.empty());
}

TEST(PythonPhase1CContractsTest, ActorSnapshotHasExpectedDefaults) {
    python::PythonActorSnapshot snap;
    EXPECT_EQ(snap.generation, 0u);
    EXPECT_EQ(snap.last_sequence, 0u);
    EXPECT_EQ(snap.handled, 0u);
    EXPECT_EQ(snap.failures, 0u);
    EXPECT_EQ(snap.restarts, 0u);
    EXPECT_EQ(snap.cancellations, 0u);
    EXPECT_EQ(snap.pending_turns, 0u);
    EXPECT_FALSE(snap.active_turn);
    EXPECT_FALSE(snap.quarantined);
    EXPECT_TRUE(snap.actor_type.empty());
}

TEST(PythonPhase1CContractsTest, RuntimeSnapshotHasHeartbeatFields) {
    python::PythonRuntimeSnapshot snap;
    EXPECT_EQ(snap.dispatch_rejected, 0u);
    EXPECT_EQ(snap.command_rejected, 0u);
    EXPECT_EQ(snap.handler_exceptions, 0u);
    EXPECT_EQ(snap.handler_cancelled, 0u);
    EXPECT_EQ(snap.stale_completions, 0u);
    EXPECT_EQ(snap.last_heartbeat_ns, 0u);
    EXPECT_EQ(snap.loop_lag_ns, 0u);
    EXPECT_FALSE(snap.ready);
}
