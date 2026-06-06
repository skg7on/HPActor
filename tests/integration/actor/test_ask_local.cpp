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

#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/request_timeout.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {
namespace {

TEST(AskLocalTest, AskManagerCreatesAndResolves) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = true;
    ActorSystem system(cfg);
    ASSERT_NE(system.ask_manager(), nullptr);
}

TEST(AskLocalTest, RegisterAskReturnsUnreadyHandle) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = true;
    ActorSystem system(cfg);

    auto reg = system.ask_manager()->register_ask(ActorId{1}, ActorAddress{},
                                                  RequestTimeout::use_default(),
                                                  std::chrono::milliseconds{5000});

    EXPECT_FALSE(reg.handle.ready());
    EXPECT_NE(reg.msg_id.value(), 0u);
}

TEST(AskLocalTest, OnResponseResolvesHandle) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = true;
    ActorSystem system(cfg);

    auto reg = system.ask_manager()->register_ask(ActorId{1}, ActorAddress{},
                                                  RequestTimeout::use_default(),
                                                  std::chrono::milliseconds{5000});

    StreamBuffer response;
    response.append(reinterpret_cast<const uint8_t*>("ok"), 2);
    bool found = system.ask_manager()->on_response(reg.msg_id.value(),
                                                   std::move(response));
    EXPECT_TRUE(found);
    EXPECT_TRUE(reg.handle.ready());
}

} // namespace
} // namespace hpactor
