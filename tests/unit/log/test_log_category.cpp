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

#include <cstring>
#include <gtest/gtest.h>
#include <hpactor/log/log_category.hpp>

using namespace hpactor::log;

TEST(LogCategoryTest, ToString) {
    EXPECT_STREQ(to_string(LogCategory::kActor), "actor");
    EXPECT_STREQ(to_string(LogCategory::kActorState), "actor_state");
    EXPECT_STREQ(to_string(LogCategory::kMailbox), "mailbox");
    EXPECT_STREQ(to_string(LogCategory::kScheduler), "scheduler");
    EXPECT_STREQ(to_string(LogCategory::kMemory), "memory");
    EXPECT_STREQ(to_string(LogCategory::kRegistrar), "registrar");
    EXPECT_STREQ(to_string(LogCategory::kDiscovery), "discovery");
    EXPECT_STREQ(to_string(LogCategory::kNetwork), "network");
    EXPECT_STREQ(to_string(LogCategory::kRpc), "rpc");
    EXPECT_STREQ(to_string(LogCategory::kConfig), "config");
    EXPECT_STREQ(to_string(LogCategory::kSupervision), "supervision");
    EXPECT_STREQ(to_string(LogCategory::kCli), "cli");
    EXPECT_STREQ(to_string(LogCategory::kHttp), "http");
    EXPECT_STREQ(to_string(LogCategory::kUser), "user");
}

TEST(LogCategoryTest, EventIdToString) {
    EXPECT_STREQ(to_string(LogEventId::kActorSpawned), "actor_spawned");
    EXPECT_STREQ(to_string(LogEventId::kActorTerminated), "actor_terminated");
    EXPECT_STREQ(to_string(LogEventId::kActorStateTransfer), "actor_state_"
                                                             "transfer");
    EXPECT_STREQ(to_string(LogEventId::kActorLinkRejected), "actor_link_"
                                                            "rejected");
    EXPECT_STREQ(to_string(LogEventId::kMailboxDepthHigh), "mailbox_depth_"
                                                           "high");
    EXPECT_STREQ(to_string(LogEventId::kMemoryAlloc), "memory_alloc");
    EXPECT_STREQ(to_string(LogEventId::kMemoryFree), "memory_free");
    EXPECT_STREQ(to_string(LogEventId::kMemoryCorruption), "memory_corruption");
    EXPECT_STREQ(to_string(LogEventId::kRegistrarRegister), "registrar_"
                                                            "register");
    EXPECT_STREQ(to_string(LogEventId::kRegistrarResolveMiss), "registrar_"
                                                               "resolve_miss");
    EXPECT_STREQ(to_string(LogEventId::kDiscoveryNodeJoined), "discovery_node_"
                                                              "joined");
    EXPECT_STREQ(to_string(LogEventId::kDiscoveryNodeDead), "discovery_node_"
                                                            "dead");
    EXPECT_STREQ(to_string(LogEventId::kNetworkFrameReceived), "network_frame_"
                                                               "received");
    EXPECT_STREQ(to_string(LogEventId::kNetworkFrameDecodeFailed), "network_"
                                                                   "frame_"
                                                                   "decode_"
                                                                   "failed");
    EXPECT_STREQ(to_string(LogEventId::kSchedulerDispatch), "scheduler_"
                                                            "dispatch");
    EXPECT_STREQ(to_string(LogEventId::kSchedulerSteal), "scheduler_steal");
}

TEST(LogCategoryTest, UnknownEventId) {
    EXPECT_STREQ(to_string(static_cast<LogEventId>(9999)), "unknown_event");
}

TEST(LogCategoryTest, ParseCategorySuccess) {
    EXPECT_TRUE(parse_category("actor").has_value());
    EXPECT_EQ(parse_category("actor").value(), LogCategory::kActor);
    EXPECT_EQ(parse_category("actor_state").value(), LogCategory::kActorState);
    EXPECT_EQ(parse_category("mailbox").value(), LogCategory::kMailbox);
    EXPECT_EQ(parse_category("scheduler").value(), LogCategory::kScheduler);
    EXPECT_EQ(parse_category("memory").value(), LogCategory::kMemory);
    EXPECT_EQ(parse_category("registrar").value(), LogCategory::kRegistrar);
    EXPECT_EQ(parse_category("discovery").value(), LogCategory::kDiscovery);
    EXPECT_EQ(parse_category("network").value(), LogCategory::kNetwork);
    EXPECT_EQ(parse_category("rpc").value(), LogCategory::kRpc);
    EXPECT_EQ(parse_category("config").value(), LogCategory::kConfig);
    EXPECT_EQ(parse_category("supervision").value(), LogCategory::kSupervision);
    EXPECT_EQ(parse_category("cli").value(), LogCategory::kCli);
    EXPECT_EQ(parse_category("http").value(), LogCategory::kHttp);
    EXPECT_EQ(parse_category("user").value(), LogCategory::kUser);
}

TEST(LogCategoryTest, ParseCategoryFailure) {
    EXPECT_FALSE(parse_category("invalid").has_value());
}
