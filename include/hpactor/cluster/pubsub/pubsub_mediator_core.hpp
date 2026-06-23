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

#include <cstdint>
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
    /// \brief Construct with the local node identity.
    ///
    /// \param[in] node_id The local node ID used for gossip origin tagging.
    explicit PubSubMediatorCore(const std::string& node_id = "")
        : node_id_(node_id) {}

    /// \brief Subscribe a local actor to a topic.
    ///
    /// \param[in] topic The topic to subscribe to.
    /// \param[in] actor_id The local actor id to register.
    /// \return \c false if capacity exceeded (kMaxTopics or
    ///         kMaxSubscribersPerTopic).
    bool subscribe(const PubSubTopic& topic, uint64_t actor_id);

    /// \brief Unsubscribe a local actor from a topic.
    ///
    /// Removes the actor from the topic. If the topic becomes empty,
    /// it is removed from the local table.
    ///
    /// \param[in] topic The topic to unsubscribe from.
    /// \param[in] actor_id The local actor id to remove.
    void unsubscribe(const PubSubTopic& topic, uint64_t actor_id);

    /// \brief Merge a remote subscription entry from gossip.
    ///
    /// Adds or updates a remote subscription in the remote table.
    /// Only overwrites if \p sub.incarnation is higher than the
    /// existing entry.
    ///
    /// \param[in] sub The remote subscription to merge.
    void merge_remote_subscription(const TopicSubscription& sub);

    /// \brief Remove all subscriptions from a node.
    ///
    /// Called when a node goes Down via ClusterFailureModel.
    /// Also cleans up topics whose inner maps become empty.
    ///
    /// \param[in] node_id The node whose subscriptions are purged.
    void remove_node_subscriptions(const std::string& node_id);

    /// \brief Get all local subscriber actor IDs for a topic.
    ///
    /// \param[in] topic The topic to query.
    /// \return Vector of local subscriber actor IDs (empty if none).
    std::vector<uint64_t> local_subscribers_for(const PubSubTopic& topic) const;

    /// \brief Get all subscriber entries (local + remote) for a topic.
    ///
    /// \param[in] topic The topic to query.
    /// \return Combined vector of local and remote TopicSubscription entries.
    ///         Local entries carry \c subscriber_node set to this node's ID.
    std::vector<TopicSubscription>
    all_subscribers_for(const PubSubTopic& topic) const;

    /// \brief Number of local topics with at least one subscriber.
    ///
    /// \return Current local topic count.
    size_t topic_count() const;

    /// \brief Number of local subscribers for a topic.
    ///
    /// \param[in] topic The topic to query.
    /// \return Number of local subscribers (0 if none).
    size_t subscriber_count(const PubSubTopic& topic) const;

    /// \brief Drain dirty subscriptions that need gossip dissemination.
    ///
    /// Returns subscriptions added since the last drain and clears the
    /// internal dirty set.
    ///
    /// \return Vector of dirty TopicSubscription entries.
    std::vector<TopicSubscription> drain_dirty_subscriptions();

  private:
    mutable std::mutex mutex_;
    std::string node_id_;
    uint64_t incarnation_{0};

    // Local: topic → set of actor_ids
    std::unordered_map<PubSubTopic, std::unordered_set<uint64_t>> local_subs_;

    // Remote: topic → node_id → subscription
    std::unordered_map<PubSubTopic, std::unordered_map<std::string, TopicSubscription>> remote_subs_;

    // Dirty local subscriptions needing gossip dissemination
    std::vector<TopicSubscription> dirty_subs_;
};

} // namespace hpactor::cluster::pubsub
