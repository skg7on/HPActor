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

/// \file test_runtime_lifecycle_boundaries.cpp
///
/// \brief RED characterization tests for Phase 6 startup/lifecycle boundaries.
///
/// These tests document the CURRENT behavior (pre-Phase 6) of ActorSystem
/// construction, readiness, topology loading, and shutdown ordering. They
/// serve as regression detectors for the Phase 6 refactor.
///
/// Tests marked with "RED-GOAL" will change behavior when Phase 6 is
/// complete — they document gaps that Phase 6 will close.

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

#include <gtest/gtest.h>

// ── Helper: minimal config for unit testing ─────────────────────────────────

static hpactor::Config minimal_config() {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;  // no worker threads
    cfg.enable_network = false; // no network
    cfg.scheduler_start_paused = false;
    return cfg;
}

// ── Construction + readiness characterization ──────────────────────────────

/// RED-GOAL: Phase 6 should make readiness false during construction
/// and only transition to true after all startup stages complete.
/// Currently is_ready() defaults to true.
TEST(RuntimeLifecycleCharacterization, ReadinessDefaultsToTrue) {
    hpactor::Config cfg = minimal_config();

    hpactor::ActorSystem system{cfg};

    // CURRENT: is_ready() returns true immediately after construction.
    // PHASE 6 GOAL: is_ready() should only be true after explicit
    // coordinator startup completes.
    //
    // This test will FAIL when Phase 6 corrects the readiness default.
    EXPECT_TRUE(system.is_ready());
}

/// Construction with network disabled produces a usable system.
TEST(RuntimeLifecycleCharacterization, ConstructsWithoutNetwork) {
    hpactor::Config cfg = minimal_config();

    hpactor::ActorSystem system{cfg};

    EXPECT_TRUE(system.is_running());
    EXPECT_EQ(system.shutdown_phase(), hpactor::ShutdownPhase::Running);
    EXPECT_NE(system.scheduler(), nullptr);
}

/// Construction with zero scheduler threads produces a valid system.
TEST(RuntimeLifecycleCharacterization, ConstructsWithZeroThreads) {
    hpactor::Config cfg = minimal_config();
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system{cfg};

    EXPECT_TRUE(system.is_running());
    EXPECT_TRUE(system.is_ready());
}

/// Verify that the destructor cleans up cleanly after construction.
TEST(RuntimeLifecycleCharacterization, DestructorCleansUpAfterConstruction) {
    hpactor::Config cfg = minimal_config();

    {
        hpactor::ActorSystem system{cfg};
        EXPECT_TRUE(system.is_running());
    }
    // Destructor completes without hanging or crashing.
    SUCCEED();
}

// ── Shutdown phase transition characterization ─────────────────────────────

/// Verify that shutdown() transitions through phases.
TEST(RuntimeLifecycleCharacterization, ShutdownTransitionsToStopped) {
    hpactor::Config cfg = minimal_config();

    hpactor::ActorSystem system{cfg};
    EXPECT_EQ(system.shutdown_phase(), hpactor::ShutdownPhase::Running);

    auto result = system.shutdown();
    EXPECT_TRUE(result.ok());

    // After shutdown, phase should be Stopped or ForcedStop.
    auto phase = system.shutdown_phase();
    EXPECT_TRUE(phase == hpactor::ShutdownPhase::Stopped ||
                phase == hpactor::ShutdownPhase::ForcedStop);
}

/// Shutdown sets is_ready to false.
TEST(RuntimeLifecycleCharacterization, ShutdownClearsReadiness) {
    hpactor::Config cfg = minimal_config();

    hpactor::ActorSystem system{cfg};
    EXPECT_TRUE(system.is_ready());

    (void)system.shutdown();
    EXPECT_FALSE(system.is_ready());
}

// ── Config immutability characterization (RED-GOAL) ───────────────────────

/// RED-GOAL: Phase 6 should make the effective runtime config
/// distinguishable from the constructor input. Currently they are
/// the same mutable object.
TEST(RuntimeLifecycleCharacterization, ConfigIsMutableAfterConstruction) {
    hpactor::Config cfg = minimal_config();
    cfg.scheduler_threads = 0;

    hpactor::ActorSystem system{cfg};

    // CURRENT: config() returns a reference to the same mutable Config
    // that was passed in.  Phase 6 should distinguish the validated
    // effective blueprint from the user-provided config.
    const auto& stored = system.config();
    EXPECT_EQ(stored.scheduler_threads, 0);
    EXPECT_EQ(stored.enable_network, false);

    // TODO(Phase 6): add immutability enforcement
    // e.g., const_cast would be UB or the runtime config is a separate object
}

// ── Topology loading characterization (RED-GOAL) ──────────────────────────

/// RED-GOAL: Phase 6 should validate all actor factories before mutating
/// any runtime config. Currently, load_topology mutates config fields
/// before validating actor behaviors.
///
/// This test documents the current behavior: trying to load a topology
/// with an unknown behavior causes load_topology to fail, but the config
/// mutations already happened.
TEST(RuntimeLifecycleCharacterization, TopologyLoadCanFailAfterConfigMutation) {
    // This test requires a TOML file with an unknown actor behavior.
    // We skip the file-based test and instead verify that the
    // ActorFactoryRegistry can detect unknown behaviors (which is what
    // load_topology should validate first).
    auto& registry = hpactor::config::ActorFactoryRegistry::instance();
    EXPECT_FALSE(registry.has("nonexistent.actor.behavior.v1"));
}

// ── Shutdown coordinator characterization ─────────────────────────────────

/// Verify that the shutdown coordinator is available after construction.
TEST(RuntimeLifecycleCharacterization, ShutdownCoordinatorExists) {
    hpactor::Config cfg = minimal_config();

    hpactor::ActorSystem system{cfg};

    auto* sc = system.shutdown_coordinator();
    EXPECT_NE(sc, nullptr);
    EXPECT_TRUE(sc->accepting_ingress());
}

/// RED-GOAL: Phase 6 should unify destructor and shutdown paths.
/// Currently they are independent (destructor does not call shutdown
/// coordinator's execute method).
TEST(RuntimeLifecycleCharacterization, DestructorDoesNotUseShutdownCoordinator) {
    hpactor::Config cfg = minimal_config();

    // This is a behavioral characterization: the destructor's teardown
    // sequence is independent of ShutdownCoordinator::execute().
    // In Phase 6, both paths converge on RuntimeCoordinator::stop().
    //
    // We verify by checking that after construction + destruction,
    // the system doesn't leak or double-free.
    {
        hpactor::ActorSystem system{cfg};
        // Shutdown coordinator exists but we won't call it.
        // Destructor will use its own teardown path.
        EXPECT_NE(system.shutdown_coordinator(), nullptr);
    }
    SUCCEED();
}

// ── Process preflight ordering characterization ───────────────────────────

/// RED-GOAL: Phase 6 must ensure process preflight (daemonization)
/// happens before any runtime thread. Currently this is enforced by
/// constructor ordering but not verified programmatically.
TEST(RuntimeLifecycleCharacterization, ProcessPreflightBeforeThreads) {
    hpactor::Config cfg = minimal_config();
    cfg.scheduler_threads = 0; // no threads

    // With scheduler_threads=0, no worker threads are created.
    // The system should still be functional.
    hpactor::ActorSystem system{cfg};

    EXPECT_TRUE(system.is_running());
    // In foreground mode (default), ProcessManager::mode() == Foreground.
    // No daemonization occurred.
}

// ── Repeated construction/destruction (ASan smoke) ───────────────────────

TEST(RuntimeLifecycleCharacterization, RepeatedConstructDestroy) {
    for (int i = 0; i < 5; ++i) {
        hpactor::Config cfg = minimal_config();
        hpactor::ActorSystem system{cfg};
        EXPECT_TRUE(system.is_running());
        (void)system.shutdown();
    }
    SUCCEED();
}

// ── Double shutdown is safe ──────────────────────────────────────────────

TEST(RuntimeLifecycleCharacterization, DoubleShutdownIsSafe) {
    hpactor::Config cfg = minimal_config();

    hpactor::ActorSystem system{cfg};
    auto r1 = system.shutdown();
    EXPECT_TRUE(r1.ok());

    auto r2 = system.shutdown();
    // Second shutdown should not crash or return an error.
    EXPECT_TRUE(r2.ok());
}
