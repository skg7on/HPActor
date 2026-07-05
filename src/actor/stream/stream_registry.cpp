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

#include <hpactor/actor/stream/stream_registry.hpp>

namespace hpactor {

void StreamRegistry::register_sender(uint64_t stream_id, ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    senders_[stream_id] = actor_id;
}

void StreamRegistry::register_receiver(uint64_t stream_id, ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    receivers_[stream_id] = actor_id;
}

std::optional<ActorId> StreamRegistry::find_sender(uint64_t stream_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = senders_.find(stream_id);
    return it == senders_.end() ? std::nullopt : std::optional<ActorId>{it->second};
}

std::optional<ActorId> StreamRegistry::find_receiver(uint64_t stream_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = receivers_.find(stream_id);
    return it == receivers_.end() ? std::nullopt
                                  : std::optional<ActorId>{it->second};
}

StreamRoutes StreamRegistry::take(uint64_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamRoutes routes;
    if (auto it = senders_.find(stream_id); it != senders_.end()) {
        routes.sender = it->second;
        senders_.erase(it);
    }
    if (auto it = receivers_.find(stream_id); it != receivers_.end()) {
        routes.receiver = it->second;
        receivers_.erase(it);
    }
    return routes;
}

std::size_t StreamRegistry::sender_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return senders_.size();
}

std::size_t StreamRegistry::receiver_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receivers_.size();
}

} // namespace hpactor
