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

#include <cstdint>
#include <string>

namespace hpactor::cluster::pubsub {

/// \brief A pub-sub topic identifier.
///
/// Topics are string-based names that actors publish to and subscribe to.
/// The PubSubMediator fans out published messages to all subscribers.
struct PubSubTopic {
    std::string name;

    /// \brief Equality comparison based on the topic name.
    bool operator==(const PubSubTopic& other) const {
        return name == other.name;
    }
};

/// \brief A subscription entry representing a subscriber on a specific node.
struct TopicSubscription {
    PubSubTopic topic;
    std::string subscriber_node;  ///< Node where subscriber actor lives.
    uint64_t subscriber_actor_id; ///< ActorId of the subscriber actor.
    uint64_t incarnation = 0;     ///< For conflict resolution.

    /// \brief Equality comparison based on topic, node, and actor id.
    bool operator==(const TopicSubscription& other) const {
        return topic == other.topic && subscriber_node == other.subscriber_node &&
               subscriber_actor_id == other.subscriber_actor_id &&
               incarnation == other.incarnation;
    }
};

/// \brief Maximum number of topics per PubSubMediatorCore instance.
constexpr size_t kMaxTopics = 4096;

/// \brief Maximum number of subscribers per topic.
constexpr size_t kMaxSubscribersPerTopic = 1024;

} // namespace hpactor::cluster::pubsub

// Hash support for PubSubTopic in unordered containers.
namespace std {
/// \brief std::hash specialization for PubSubTopic.
template <> struct hash<::hpactor::cluster::pubsub::PubSubTopic> {
    /// \brief Hash a PubSubTopic by its name string.
    ///
    /// \param[in] t The topic to hash.
    /// \return Hash of the topic name.
    size_t
    operator()(const ::hpactor::cluster::pubsub::PubSubTopic& t) const noexcept {
        return hash<string>()(t.name);
    }
};
} // namespace std
