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

#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/python/python_runtime_config.hpp>
#include <hpactor/python/python_type_tags.hpp>

using namespace hpactor;

TEST(PythonContractsTest, TagsUseReservedLocalRange) {
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonWakeupTag), 0xF0u);
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonActorCommandTag), 0xF1u);
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonActorFailedTag), 0xF2u);
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonInspectTag), 0xF3u);
}

TEST(PythonContractsTest, ConfigRejectsInvalidBounds) {
    python::PythonRuntimeConfig cfg;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::None);

    cfg.dispatch_queue_capacity = 63;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::QueueCapacity);
    cfg = {};
    cfg.command_queue_capacity = 65;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::QueueCapacity);
    cfg = {};
    cfg.completion_queue_capacity = 1'048'577;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::QueueCapacity);
    cfg = {};
    cfg.max_dispatch_per_tick = 0;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::DrainBudget);
    cfg = {};
    cfg.max_commands_per_turn = cfg.command_queue_capacity + 1;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::DrainBudget);
    cfg = {};
    cfg.dispatch_queue_capacity = 64;
    cfg.max_dispatch_per_tick = 128; // within [1,4096] but > queue capacity
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::DrainBudget);
    cfg = {};
    cfg.max_actor_bindings = 0;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ActorBindingCapacity);
    cfg = {};
    cfg.max_actor_bindings = 1'048'577;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ActorBindingCapacity);
    cfg = {};
    cfg.loop_lag_unready_ms = 99;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::LoopLag);
    cfg = {};
    cfg.loop_lag_unready_ms = 60'001;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::LoopLag);
    cfg = {};
    cfg.handler_shutdown_timeout_ms = 99;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ShutdownTimeout);
    cfg = {};
    cfg.handler_shutdown_timeout_ms = 300'001;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ShutdownTimeout);
}

TEST(PythonContractsTest, LanguageBindingFailureSourceIsAppendOnly) {
    EXPECT_EQ(static_cast<uint8_t>(FailureSource::Unknown), 11u);
    EXPECT_EQ(static_cast<uint8_t>(FailureSource::LanguageBinding), 12u);
    EXPECT_STREQ(to_string(FailureSource::LanguageBinding), "language_binding");
}
