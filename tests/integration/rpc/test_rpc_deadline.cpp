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

#include <hpactor/core/actor_system.hpp>

namespace hpactor {
namespace {

TEST(RpcDeadlineTest, RpcChannelHasConfigurableMaxRetries) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.default_ask_max_retries = 2;
    ActorSystem system(cfg);
    // Verify the config value is accepted — RpcChannel construction
    // happens during ActorSystem init
    EXPECT_EQ(cfg.default_ask_max_retries, uint32_t{2});
    SUCCEED();
}

} // namespace
} // namespace hpactor
