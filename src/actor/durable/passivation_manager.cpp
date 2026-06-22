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

#include <hpactor/actor/durable/passivation_manager.hpp>

#include <mutex>

namespace hpactor::actor::durable {

void PassivationManager::register_actor(ActorId actor_id,
                                        const std::string& persistence_id,
                                        const PassivationConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    actors_[actor_id.value()] =
        ActorRecord{persistence_id, config, PassivationState::Active};
}

void PassivationManager::unregister_actor(ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    actors_.erase(actor_id.value());
}

bool PassivationManager::is_tracked(ActorId actor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return actors_.find(actor_id.value()) != actors_.end();
}

bool PassivationManager::begin_passivate(ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actors_.find(actor_id.value());
    if (it == actors_.end() || it->second.state != PassivationState::Active)
        return false;
    it->second.state = PassivationState::Passivating;
    return true;
}

void PassivationManager::complete_passivation(ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    actors_.erase(actor_id.value());
}

PassivationState PassivationManager::get_state(ActorId actor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actors_.find(actor_id.value());
    if (it == actors_.end())
        return PassivationState::Passivated;
    return it->second.state;
}

size_t PassivationManager::tracked_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return actors_.size();
}

} // namespace hpactor::actor::durable
