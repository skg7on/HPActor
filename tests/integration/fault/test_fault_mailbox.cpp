// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/fault/fault_point.hpp>
#include <hpactor/fault/fault_schedule.hpp>
#include <hpactor/hpactor_config.hpp>

#include <gtest/gtest.h>

namespace hpactor::fault {
namespace {

TEST(FaultMailboxIntegration, ActorSystemInstallsThreadLocal) {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    ::hpactor::ActorSystem system(cfg);

    // After ActorSystem construction, the thread-local controller is set
    auto* fc = FaultController::instance();
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc, &system.fault_controller());
}

TEST(FaultMailboxIntegration, FaultControllerDisabledByDefault) {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    ::hpactor::ActorSystem system(cfg);

    auto& fc = system.fault_controller();
    EXPECT_FALSE(fc.is_enabled());

    // FAULT_INJECT should be a no-op when disabled
    bool injected = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        injected = true;
    }
    EXPECT_FALSE(injected);
    EXPECT_EQ(fc.faults_fired(), 0u);
}

#if HPACTOR_ENABLE_FAULT_INJECTION
TEST(FaultMailboxIntegration, EnqueueFailFiresWithSchedule) {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    ::hpactor::ActorSystem system(cfg);

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");

    // First call: tick advances 0→1, matches schedule at tick 1
    bool injected = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        injected = true;
    }
    EXPECT_TRUE(injected);
    EXPECT_EQ(fc.faults_fired(), 1u);
}

TEST(FaultMailboxIntegration, DequeueDropFiresWithSchedule) {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    ::hpactor::ActorSystem system(cfg);

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.dequeue.drop",
                        FaultAction::kDrop, std::nullopt, std::monostate{}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");

    bool injected = false;
    FAULT_INJECT("hpactor.mailbox.dequeue.drop") {
        injected = true;
    }
    EXPECT_TRUE(injected);
    EXPECT_EQ(fc.faults_fired(), 1u);
}

TEST(FaultMailboxIntegration, ScopeFiltersFaultPath) {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    ::hpactor::ActorSystem system(cfg);

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("hpactor.transport.*"); // wrong scope

    bool injected = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        injected = true;
    }
    EXPECT_FALSE(injected);
    EXPECT_EQ(fc.faults_fired(), 0u);
}

TEST(FaultMailboxIntegration, ClearStopsInjection) {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    ::hpactor::ActorSystem system(cfg);

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kMailbox, 1, "hpactor.mailbox.enqueue.fail",
                        FaultAction::kFail, std::nullopt, FailPayload{-1}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");
    fc.clear();

    bool injected = false;
    FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
        injected = true;
    }
    EXPECT_FALSE(injected);
    EXPECT_EQ(fc.faults_fired(), 0u);
}

TEST(FaultMailboxIntegration, TransportSendDropFiresWithSchedule) {
    ::hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    ::hpactor::ActorSystem system(cfg);

    FaultSchedule schedule;
    schedule.add_entry({FaultDomain::kTransport, 1, "hpactor.transport.send.drop",
                        FaultAction::kDrop, std::nullopt, std::monostate{}});

    auto& fc = system.fault_controller();
    fc.load(schedule);
    fc.enable("*");

    bool injected = false;
    FAULT_INJECT("hpactor.transport.send.drop") {
        injected = true;
    }
    EXPECT_TRUE(injected);
    EXPECT_EQ(fc.faults_fired(), 1u);
}

#endif // HPACTOR_ENABLE_FAULT_INJECTION

} // anonymous namespace
} // namespace hpactor::fault
