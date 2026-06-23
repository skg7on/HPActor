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

#include <hpactor/cluster/pubsub/pubsub_mediator_core.hpp>

#include <string>
#include <vector>

namespace hpactor::cluster::pubsub {

/// \brief Actor wrapper around PubSubMediatorCore.
///
/// Thin delegate-to-core wrapper following the Core/Actor separation
/// pattern. The core is thread-safe and directly testable; this wrapper
/// provides the actor-facing API for cluster event integration (gossip
/// piggyback, node-down cleanup, periodic dirty-set drain).
///
/// Future evolution: When cluster mode is fully wired, this becomes an
/// EventBasedActor with TypeTag-based message dispatch for Subscribe,
/// Unsubscribe, and Publish operations.
///
/// \note Delegates to a thread-safe core; callers must still synchronize
///       access when sharing the wrapper across threads.
class PubSubMediatorActor {
  public:
    /// \brief Default-construct with an empty core.
    PubSubMediatorActor() : core_() {}

    /// \brief Access the underlying core (mutable).
    ///
    /// \return Reference to the PubSubMediatorCore.
    PubSubMediatorCore& core() {
        return core_;
    }

    /// \brief Access the underlying core (const).
    ///
    /// \return Const reference to the PubSubMediatorCore.
    const PubSubMediatorCore& core() const {
        return core_;
    }

    // --- Delegates to core ---

    /// \brief Subscribe a local actor to a topic (delegates to core).
    ///
    /// \param[in] topic The topic to subscribe to.
    /// \param[in] actor_id The local actor id.
    /// \return \c false if capacity exceeded.
    bool subscribe(const PubSubTopic& topic, uint64_t actor_id) {
        return core_.subscribe(topic, actor_id);
    }

    /// \brief Unsubscribe a local actor from a topic (delegates to core).
    ///
    /// \param[in] topic The topic to unsubscribe from.
    /// \param[in] actor_id The local actor id.
    void unsubscribe(const PubSubTopic& topic, uint64_t actor_id) {
        core_.unsubscribe(topic, actor_id);
    }

    /// \brief Merge a remote subscription from gossip (delegates to core).
    ///
    /// \param[in] sub The remote subscription to merge.
    void merge_remote_subscription(const TopicSubscription& sub) {
        core_.merge_remote_subscription(sub);
    }

    /// \brief Remove all subscriptions from a node (delegates to core).
    ///
    /// \param[in] node_id The node whose subscriptions are purged.
    void remove_node_subscriptions(const std::string& node_id) {
        core_.remove_node_subscriptions(node_id);
    }

    /// \brief Get local subscribers for a topic (delegates to core).
    ///
    /// \param[in] topic The topic to query.
    /// \return Vector of local subscriber actor IDs.
    std::vector<uint64_t> local_subscribers_for(const PubSubTopic& topic) const {
        return core_.local_subscribers_for(topic);
    }

    /// \brief Get all subscribers for a topic (delegates to core).
    ///
    /// \param[in] topic The topic to query.
    /// \return Combined vector of local and remote TopicSubscription entries.
    std::vector<TopicSubscription>
    all_subscribers_for(const PubSubTopic& topic) const {
        return core_.all_subscribers_for(topic);
    }

    /// \brief Get local topic count (delegates to core).
    ///
    /// \return Number of local topics.
    size_t topic_count() const {
        return core_.topic_count();
    }

    /// \brief Get subscriber count for a topic (delegates to core).
    ///
    /// \param[in] topic The topic to query.
    /// \return Number of local subscribers.
    size_t subscriber_count(const PubSubTopic& topic) const {
        return core_.subscriber_count(topic);
    }

    /// \brief Drain dirty subscriptions for gossip (delegates to core).
    ///
    /// \return Vector of dirty TopicSubscription entries.
    std::vector<TopicSubscription> drain_dirty_subscriptions() {
        return core_.drain_dirty_subscriptions();
    }

  private:
    PubSubMediatorCore core_;
};

} // namespace hpactor::cluster::pubsub
