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
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/ref/actor_proxy.hpp>

using namespace hpactor;

class ActorProxyDeadLettersTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg_.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg_.enable_network = false;
        system_ = std::make_unique<ActorSystem>(cfg_);
    }

    void TearDown() override {
        system_.reset();
    }

    Config cfg_;
    std::unique_ptr<ActorSystem> system_;
};

TEST_F(ActorProxyDeadLettersTest, RemoteNodeUnreachableGeneratesDeadLetter) {
    // Create a proxy to a remote actor (no network, so transport will be null)
    ActorAddress remote{endpoint_ops::parse_endpoint("10.0.0.1:9000"),
                        ActorType{1}, ActorId{44}, 0};
    ActorProxy proxy(remote, system_.get());

    auto result =
        proxy.try_send(remote, TypedMessage(TypeTag::User, StreamBuffer{9}));
    EXPECT_FALSE(result.accepted());

    // Verify dead letter was captured with RemoteNodeUnreachable
    mailbox::DeadLetterRecord dl;
    ASSERT_TRUE(system_->pop_dead_letter(dl));
    EXPECT_EQ(dl.reason, mailbox::DeadLetterReason::RemoteNodeUnreachable);
    EXPECT_EQ(dl.source, mailbox::DeadLetterSource::ActorProxy);
    EXPECT_EQ(dl.target.id, ActorId{44});

    // No more dead letters
    EXPECT_FALSE(system_->pop_dead_letter(dl));
}
