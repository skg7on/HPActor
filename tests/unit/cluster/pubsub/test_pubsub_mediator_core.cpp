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
#include <hpactor/cluster/pubsub/pubsub_mediator_core.hpp>

namespace hpactor::cluster::pubsub {

TEST(PubSubMediatorCoreTest, SubscribeAddsSubscriber) {
    PubSubMediatorCore core;
    PubSubTopic topic{"test-topic"};
    EXPECT_TRUE(core.subscribe(topic, 42));
    EXPECT_EQ(core.subscriber_count(topic), 1);
}

TEST(PubSubMediatorCoreTest, SubscribeReturnsFalseWhenTopicLimitReached) {
    PubSubMediatorCore core;
    // Fill up to kMaxTopics
    for (size_t i = 0; i < kMaxTopics; ++i) {
        PubSubTopic topic{"topic-" + std::to_string(i)};
        EXPECT_TRUE(core.subscribe(topic, 1));
    }
    // One more should fail
    PubSubTopic overflow{"overflow-topic"};
    EXPECT_FALSE(core.subscribe(overflow, 1));
}

TEST(PubSubMediatorCoreTest, SubscribeReturnsFalseWhenSubscriberLimitReached) {
    PubSubMediatorCore core;
    PubSubTopic topic{"full-topic"};
    // Fill up to kMaxSubscribersPerTopic
    for (size_t i = 0; i < kMaxSubscribersPerTopic; ++i) {
        EXPECT_TRUE(core.subscribe(topic, static_cast<uint64_t>(i + 100)));
    }
    // One more should fail
    EXPECT_FALSE(core.subscribe(topic, 9999));
}

TEST(PubSubMediatorCoreTest, UnsubscribeRemovesSubscriber) {
    PubSubMediatorCore core;
    PubSubTopic topic{"test-topic"};
    core.subscribe(topic, 42);
    core.subscribe(topic, 43);
    EXPECT_EQ(core.subscriber_count(topic), 2);

    core.unsubscribe(topic, 42);
    EXPECT_EQ(core.subscriber_count(topic), 1);
}

TEST(PubSubMediatorCoreTest, UnsubscribeLastSubscriberRemovesTopic) {
    PubSubMediatorCore core;
    PubSubTopic topic{"test-topic"};
    core.subscribe(topic, 42);
    EXPECT_EQ(core.topic_count(), 1);

    core.unsubscribe(topic, 42);
    EXPECT_EQ(core.topic_count(), 0);
}

TEST(PubSubMediatorCoreTest, UnsubscribeNonexistentDoesNotCrash) {
    PubSubMediatorCore core;
    PubSubTopic topic{"no-such-topic"};
    core.unsubscribe(topic, 99);
    EXPECT_EQ(core.topic_count(), 0);
}

TEST(PubSubMediatorCoreTest, LocalSubscribersForReturnsCorrectActors) {
    PubSubMediatorCore core;
    PubSubTopic topic{"test-topic"};
    core.subscribe(topic, 10);
    core.subscribe(topic, 20);
    core.subscribe(topic, 30);

    auto subs = core.local_subscribers_for(topic);
    EXPECT_EQ(subs.size(), 3);
}

TEST(PubSubMediatorCoreTest, LocalSubscribersForEmptyTopicReturnsEmpty) {
    PubSubMediatorCore core;
    PubSubTopic topic{"empty-topic"};
    auto subs = core.local_subscribers_for(topic);
    EXPECT_TRUE(subs.empty());
}

TEST(PubSubMediatorCoreTest, MergeRemoteSubscriptionAddsRemoteSub) {
    PubSubMediatorCore core;
    TopicSubscription sub{PubSubTopic{"remote-topic"}, "node-b", 100, 1};
    core.merge_remote_subscription(sub);

    auto all = core.all_subscribers_for(PubSubTopic{"remote-topic"});
    EXPECT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].subscriber_node, "node-b");
    EXPECT_EQ(all[0].subscriber_actor_id, 100);
}

TEST(PubSubMediatorCoreTest, AllSubscribersCombinesLocalAndRemote) {
    PubSubMediatorCore core;
    PubSubTopic topic{"mixed-topic"};
    core.subscribe(topic, 42);

    TopicSubscription remote{PubSubTopic{"mixed-topic"}, "node-x", 200, 1};
    core.merge_remote_subscription(remote);

    auto all = core.all_subscribers_for(topic);
    EXPECT_EQ(all.size(), 2); // 1 local + 1 remote
}

TEST(PubSubMediatorCoreTest, RemoveNodeSubscriptionsClearsRemoteEntries) {
    PubSubMediatorCore core;
    PubSubTopic topic{"node-topic"};

    TopicSubscription sub1{PubSubTopic{"node-topic"}, "node-a", 1, 0};
    TopicSubscription sub2{PubSubTopic{"node-topic"}, "node-b", 2, 0};
    core.merge_remote_subscription(sub1);
    core.merge_remote_subscription(sub2);

    core.remove_node_subscriptions("node-a");
    auto all = core.all_subscribers_for(topic);
    EXPECT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].subscriber_node, "node-b");
}

TEST(PubSubMediatorCoreTest, DrainDirtySubscriptionsCapturesNewSubs) {
    PubSubMediatorCore core;
    PubSubTopic topic{"dirty-topic"};
    core.subscribe(topic, 55);

    auto dirty = core.drain_dirty_subscriptions();
    EXPECT_GE(dirty.size(), 1);
}

TEST(PubSubMediatorCoreTest, DrainDirtySubscriptionsClearsAfterDrain) {
    PubSubMediatorCore core;
    PubSubTopic topic{"dirty-topic"};
    core.subscribe(topic, 55);
    core.drain_dirty_subscriptions();

    auto second = core.drain_dirty_subscriptions();
    EXPECT_TRUE(second.empty());
}

TEST(PubSubMediatorCoreTest, MultipleTopicsAreIndependent) {
    PubSubMediatorCore core;
    PubSubTopic topic_a{"topic-a"};
    PubSubTopic topic_b{"topic-b"};

    core.subscribe(topic_a, 1);
    core.subscribe(topic_b, 2);

    EXPECT_EQ(core.subscriber_count(topic_a), 1);
    EXPECT_EQ(core.subscriber_count(topic_b), 1);
    EXPECT_EQ(core.topic_count(), 2);
}

} // namespace hpactor::cluster::pubsub
