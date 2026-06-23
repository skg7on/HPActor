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
class PubSubMediatorActor {
  public:
    PubSubMediatorActor() : core_() {}

    PubSubMediatorCore& core() {
        return core_;
    }
    const PubSubMediatorCore& core() const {
        return core_;
    }

    // --- Delegates to core ---

    bool subscribe(const PubSubTopic& topic, uint64_t actor_id) {
        return core_.subscribe(topic, actor_id);
    }

    void unsubscribe(const PubSubTopic& topic, uint64_t actor_id) {
        core_.unsubscribe(topic, actor_id);
    }

    void merge_remote_subscription(const TopicSubscription& sub) {
        core_.merge_remote_subscription(sub);
    }

    void remove_node_subscriptions(const std::string& node_id) {
        core_.remove_node_subscriptions(node_id);
    }

    std::vector<uint64_t> local_subscribers_for(const PubSubTopic& topic) const {
        return core_.local_subscribers_for(topic);
    }

    std::vector<TopicSubscription>
    all_subscribers_for(const PubSubTopic& topic) const {
        return core_.all_subscribers_for(topic);
    }

    size_t topic_count() const {
        return core_.topic_count();
    }

    size_t subscriber_count(const PubSubTopic& topic) const {
        return core_.subscriber_count(topic);
    }

    std::vector<TopicSubscription> drain_dirty_subscriptions() {
        return core_.drain_dirty_subscriptions();
    }

  private:
    PubSubMediatorCore core_;
};

} // namespace hpactor::cluster::pubsub
