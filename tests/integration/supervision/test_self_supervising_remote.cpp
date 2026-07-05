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
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/supervision/supervision.hpp>

using namespace hpactor;

namespace {

ActorRef make_remote_ref(EndPoint ep, ActorId id) {
    ActorAddress addr(ep, 0, id, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    return ActorRef(std::move(proxy));
}

} // anonymous namespace

class SelfSupervisingRemoteTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ep_ = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    }

    EndPoint ep_;
};

TEST_F(SelfSupervisingRemoteTest, AddRemoteChildIncreasesCount) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    ActorAddress addr(ep_, 0, ActorId{100}, 0);
    auto ref = make_remote_ref(ep_, ActorId{100});
    supervisor.add_remote_child(ref);

    EXPECT_EQ(supervisor.remote_children().size(), 1u);
    EXPECT_TRUE(supervisor.has_remote_child(addr));

    auto retrieved = supervisor.get_remote_child(addr);
    EXPECT_EQ(retrieved.address(), addr);
}

TEST_F(SelfSupervisingRemoteTest, HasRemoteChildFalseForUnknown) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    ActorAddress addr(ep_, 0, ActorId{200}, 0);
    EXPECT_FALSE(supervisor.has_remote_child(addr));
}

TEST_F(SelfSupervisingRemoteTest, GetRemoteChildNotFoundReturnsEmpty) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    ActorAddress addr(ep_, 0, ActorId{300}, 0);
    auto result = supervisor.get_remote_child(addr);
    EXPECT_FALSE(bool(result));
}

TEST_F(SelfSupervisingRemoteTest, RemoveRemoteChildDecreasesCount) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    ActorAddress addr(ep_, 0, ActorId{400}, 0);
    auto ref = make_remote_ref(ep_, ActorId{400});
    supervisor.add_remote_child(ref);
    EXPECT_EQ(supervisor.remote_children().size(), 1u);

    supervisor.remove_remote_child(addr);
    EXPECT_EQ(supervisor.remote_children().size(), 0u);
    EXPECT_FALSE(supervisor.has_remote_child(addr));
}

TEST_F(SelfSupervisingRemoteTest, RemoveRemoteChildNonexistentDoesNotCrash) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    ActorAddress addr(ep_, 0, ActorId{500}, 0);
    supervisor.remove_remote_child(addr);
    EXPECT_EQ(supervisor.remote_children().size(), 0u);
}

TEST_F(SelfSupervisingRemoteTest, MultipleRemoteChildren) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    for (uint64_t i = 1; i <= 5; ++i) {
        auto ref = make_remote_ref(ep_, ActorId{i});
        supervisor.add_remote_child(ref);
    }
    EXPECT_EQ(supervisor.remote_children().size(), 5u);

    ActorAddress addr3(ep_, 0, ActorId{3}, 0);
    supervisor.remove_remote_child(addr3);
    EXPECT_EQ(supervisor.remote_children().size(), 4u);
    EXPECT_FALSE(supervisor.has_remote_child(addr3));
    EXPECT_TRUE(supervisor.has_remote_child(ActorAddress(ep_, 0, ActorId{1}, 0)));
    EXPECT_TRUE(supervisor.has_remote_child(ActorAddress(ep_, 0, ActorId{5}, 0)));
}
