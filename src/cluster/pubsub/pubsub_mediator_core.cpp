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

#include <hpactor/cluster/pubsub/pubsub_mediator_core.hpp>

namespace hpactor::cluster::pubsub {

bool PubSubMediatorCore::subscribe(const PubSubTopic& topic, uint64_t actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Capacity check: max topics
    if (local_subs_.size() >= kMaxTopics &&
        local_subs_.find(topic) == local_subs_.end()) {
        return false;
    }

    auto& subs = local_subs_[topic];

    // Capacity check: max subscribers per topic
    if (subs.size() >= kMaxSubscribersPerTopic &&
        subs.find(actor_id) == subs.end()) {
        return false;
    }

    if (subs.insert(actor_id).second) {
        // New subscription — mark dirty for gossip
        TopicSubscription dirty;
        dirty.topic = topic;
        dirty.subscriber_actor_id = actor_id;
        dirty.incarnation = 0;
        dirty_subs_.push_back(dirty);
    }
    return true;
}

void PubSubMediatorCore::unsubscribe(const PubSubTopic& topic, uint64_t actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = local_subs_.find(topic);
    if (it == local_subs_.end())
        return;

    it->second.erase(actor_id);
    if (it->second.empty()) {
        local_subs_.erase(it);
    }
}

void PubSubMediatorCore::merge_remote_subscription(const TopicSubscription& sub) {
    std::lock_guard<std::mutex> lock(mutex_);

    remote_subs_[sub.topic][sub.subscriber_node] = sub;
}

void PubSubMediatorCore::remove_node_subscriptions(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [topic, node_map] : remote_subs_) {
        node_map.erase(node_id);
    }
}

std::vector<uint64_t>
PubSubMediatorCore::local_subscribers_for(const PubSubTopic& topic) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint64_t> result;
    auto it = local_subs_.find(topic);
    if (it != local_subs_.end()) {
        result.assign(it->second.begin(), it->second.end());
    }
    return result;
}

std::vector<TopicSubscription>
PubSubMediatorCore::all_subscribers_for(const PubSubTopic& topic) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TopicSubscription> result;

    // Local subscribers
    auto lit = local_subs_.find(topic);
    if (lit != local_subs_.end()) {
        for (auto actor_id : lit->second) {
            TopicSubscription sub;
            sub.topic = topic;
            sub.subscriber_actor_id = actor_id;
            result.push_back(sub);
        }
    }

    // Remote subscribers
    auto rit = remote_subs_.find(topic);
    if (rit != remote_subs_.end()) {
        for (const auto& [node_id, sub] : rit->second) {
            result.push_back(sub);
        }
    }

    return result;
}

size_t PubSubMediatorCore::topic_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_subs_.size();
}

size_t PubSubMediatorCore::subscriber_count(const PubSubTopic& topic) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = local_subs_.find(topic);
    return it != local_subs_.end() ? it->second.size() : 0;
}

std::vector<TopicSubscription> PubSubMediatorCore::drain_dirty_subscriptions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TopicSubscription> result;
    result.swap(dirty_subs_);
    return result;
}

} // namespace hpactor::cluster::pubsub
