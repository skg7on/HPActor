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
#include <hpactor/cluster/pubsub/pubsub_mediator_actor.hpp>
#include <hpactor/cluster/pubsub/pubsub_mediator_core.hpp>

namespace hpactor::cluster::pubsub {

TEST(PubSubMediatorActorTest, DefaultConstruction) {
    PubSubMediatorActor actor;
    EXPECT_EQ(actor.topic_count(), 0);
}

TEST(PubSubMediatorActorTest, SubscribeAndLookup) {
    PubSubMediatorActor actor;
    PubSubTopic topic{"actor-topic"};

    actor.subscribe(topic, 100);

    auto subs = actor.local_subscribers_for(topic);
    EXPECT_EQ(subs.size(), 1);
    EXPECT_EQ(subs[0], 100);
}

TEST(PubSubMediatorActorTest, MultipleSubscribersOnSameTopic) {
    PubSubMediatorActor actor;
    PubSubTopic topic{"shared-topic"};

    actor.subscribe(topic, 1);
    actor.subscribe(topic, 2);
    actor.subscribe(topic, 3);

    EXPECT_EQ(actor.subscriber_count(topic), 3);
}

TEST(PubSubMediatorActorTest, UnsubscribeRemovesSubscriber) {
    PubSubMediatorActor actor;
    PubSubTopic topic{"ephemeral"};

    actor.subscribe(topic, 42);
    actor.unsubscribe(topic, 42);

    EXPECT_EQ(actor.subscriber_count(topic), 0);
}

TEST(PubSubMediatorActorTest, MergeRemoteAndQueryAll) {
    PubSubMediatorActor actor;
    PubSubTopic topic{"cross-node"};

    actor.subscribe(topic, 10); // local
    actor.merge_remote_subscription(TopicSubscription{topic, "remote-node", 200, 1});

    auto all = actor.all_subscribers_for(topic);
    EXPECT_EQ(all.size(), 2);
}

TEST(PubSubMediatorActorTest, NodeDownRemovesRemoteSubs) {
    PubSubMediatorActor actor;
    PubSubTopic topic{"persistent"};

    actor.merge_remote_subscription(TopicSubscription{topic, "node-dead", 300, 1});
    actor.merge_remote_subscription(TopicSubscription{topic, "node-alive", 400, 1});

    actor.remove_node_subscriptions("node-dead");
    auto all = actor.all_subscribers_for(topic);
    EXPECT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].subscriber_node, "node-alive");
}

TEST(PubSubMediatorActorTest, AccessCore) {
    PubSubMediatorActor actor;
    EXPECT_EQ(actor.core().topic_count(), 0);
    actor.subscribe(PubSubTopic{"via-core"}, 99);
    EXPECT_EQ(actor.core().subscriber_count(PubSubTopic{"via-core"}), 1);
}

} // namespace hpactor::cluster::pubsub
