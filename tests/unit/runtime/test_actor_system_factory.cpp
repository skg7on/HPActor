// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

/// \file test_actor_system_factory.cpp
/// \brief Tests for ActorSystem::create() result-returning factory.

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

TEST(ActorSystemFactoryTest, CreateProducesValidSystem) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto result = ActorSystem::create(cfg);
    ASSERT_TRUE(result.ok());

    auto sys = std::move(result.value());
    ASSERT_NE(sys, nullptr);
    EXPECT_TRUE(sys->is_running());
    EXPECT_NE(sys->scheduler(), nullptr);
}

TEST(ActorSystemFactoryTest, CreateReportsValidationError) {
    Config cfg;
    cfg.enable_network = true;
    cfg.tcp_port = 0; // invalid — network requires port

    auto result = ActorSystem::create(cfg);
    EXPECT_TRUE(result.is_error());
}

TEST(ActorSystemFactoryTest, CreateWithNetworkDisabled) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto result = ActorSystem::create(cfg);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value()->transport(), nullptr);
}

TEST(ActorSystemFactoryTest, FactoryAndConstructorProduceEquivalentSystems) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    auto factory_result = ActorSystem::create(cfg);
    ASSERT_TRUE(factory_result.ok());

    ActorSystem constructed(cfg);

    // Both should have scheduler and be running.
    EXPECT_NE(factory_result.value()->scheduler(), nullptr);
    EXPECT_NE(constructed.scheduler(), nullptr);
}

TEST(ActorSystemFactoryTest, RepeatedCreateIsSafe) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    for (int i = 0; i < 3; ++i) {
        auto result = ActorSystem::create(cfg);
        ASSERT_TRUE(result.ok());
    }
    SUCCEED();
}
