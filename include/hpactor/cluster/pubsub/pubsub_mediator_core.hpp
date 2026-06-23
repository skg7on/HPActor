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

#pragma once

#include <hpactor/cluster/pubsub/pubsub_messages.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hpactor::cluster::pubsub {

/// \brief Thread-safe core for the distributed pub-sub mediator.
///
/// Manages topic-to-subscriber mappings with bounded capacity.
/// Subscriptions are per-node: each node tracks which of its local
/// actors are subscribed. Remote subscriptions are merged via gossip.
///
/// \note All methods are thread-safe (protected by internal mutex).
class PubSubMediatorCore {
  public:
    /// \brief Subscribe a local actor to a topic.
    /// \return false if capacity exceeded (topic or subscriber limit).
    bool subscribe(const PubSubTopic& topic, uint64_t actor_id);

    /// \brief Unsubscribe a local actor from a topic.
    void unsubscribe(const PubSubTopic& topic, uint64_t actor_id);

    /// \brief Merge a remote subscription entry from gossip.
    void merge_remote_subscription(const TopicSubscription& sub);

    /// \brief Remove all subscriptions from a node (called when node goes
    /// Down).
    void remove_node_subscriptions(const std::string& node_id);

    /// \brief Get all local subscriber actor IDs for a topic.
    std::vector<uint64_t> local_subscribers_for(const PubSubTopic& topic) const;

    /// \brief Get all subscriber actor IDs (local + remote) for a topic.
    std::vector<TopicSubscription>
    all_subscribers_for(const PubSubTopic& topic) const;

    /// \brief Number of local topics with at least one subscriber.
    size_t topic_count() const;

    /// \brief Number of local subscribers for a topic.
    size_t subscriber_count(const PubSubTopic& topic) const;

    /// \brief Get dirty subscriptions (added/removed since last gossip sync)
    ///        that need to be gossiped to peers.
    std::vector<TopicSubscription> drain_dirty_subscriptions();

  private:
    mutable std::mutex mutex_;

    // Local: topic → set of actor_ids
    std::unordered_map<PubSubTopic, std::unordered_set<uint64_t>> local_subs_;

    // Remote: topic → node_id → subscription
    std::unordered_map<PubSubTopic, std::unordered_map<std::string, TopicSubscription>> remote_subs_;

    // Dirty local subscriptions needing gossip dissemination
    std::vector<TopicSubscription> dirty_subs_;
};

} // namespace hpactor::cluster::pubsub
