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

#include <hpactor/actor/receptionist/receptionist.hpp>

namespace hpactor::receptionist {

Receptionist::Receptionist(ActorContext* ctx, ActorSystem& sys)
    : EventBasedActor(ctx, sys) {
    become(Behavior::empty());
}

Behavior Receptionist::make_behavior() {
    return Behavior::empty();
}

void Receptionist::register_actor(const ServiceKey& key, const ActorAddress& addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    registry_[key].insert(addr);
}

void Receptionist::unregister_actor(const ServiceKey& key, const ActorAddress& addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(key);
    if (it != registry_.end()) {
        it->second.erase(addr);
        if (it->second.empty())
            registry_.erase(it);
    }
}

void Receptionist::add_subscriber(const ServiceKey& key,
                                  const ActorAddress& subscriber) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_[key].insert(subscriber);
}

void Receptionist::remove_subscriber(const ServiceKey& key,
                                     const ActorAddress& subscriber) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(key);
    if (it != subscribers_.end()) {
        it->second.erase(subscriber);
        if (it->second.empty())
            subscribers_.erase(it);
    }
}

Listing Receptionist::get_listing(const ServiceKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Listing listing;
    listing.key = key;
    auto reg_it = registry_.find(key);
    if (reg_it != registry_.end()) {
        listing.addresses.assign(reg_it->second.begin(), reg_it->second.end());
    }
    return listing;
}

size_t Receptionist::registration_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto& [k, actors] : registry_)
        count += actors.size();
    return count;
}

size_t Receptionist::subscriber_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto& [k, subs] : subscribers_)
        count += subs.size();
    return count;
}

} // namespace hpactor::receptionist
