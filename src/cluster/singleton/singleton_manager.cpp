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

#include <hpactor/cluster/singleton/singleton_manager.hpp>

namespace hpactor::cluster::singleton {

SingletonManagerCore::SingletonManagerCore(std::string self_node,
                                           std::unique_ptr<ISingletonElection> election)
    : self_node_(std::move(self_node)), election_(std::move(election)) {}

const std::string& SingletonManagerCore::self_node() const {
    return self_node_;
}

void SingletonManagerCore::register_singleton(const SingletonIdentity& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (singletons_.find(id.name) != singletons_.end())
        return;
    singletons_[id.name] = {id, SingletonState::Standby};
}

void SingletonManagerCore::on_node_state_change(const std::vector<std::string>& alive_nodes) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [name, record] : singletons_) {
        auto winner = election_->elect(record.identity, alive_nodes);

        if (winner.has_value() && *winner == self_node_) {
            // Self won — activate if not already active
            if (record.state == SingletonState::Standby) {
                record.state = SingletonState::Activating;
            }
            if (record.state == SingletonState::Activating) {
                record.state = SingletonState::Active;
                record.identity.fencing_token++;
            }
        } else {
            // Self not winner — go to Standby if currently Active/Draining
            if (record.state == SingletonState::Active ||
                record.state == SingletonState::Draining) {
                record.state = SingletonState::Standby;
            }
        }
    }
}

SingletonState SingletonManagerCore::get_state(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = singletons_.find(name);
    if (it == singletons_.end())
        return SingletonState::Standby;
    return it->second.state;
}

uint64_t SingletonManagerCore::get_fencing_token(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = singletons_.find(name);
    if (it == singletons_.end())
        return 0;
    return it->second.identity.fencing_token;
}

bool SingletonManagerCore::begin_drain(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = singletons_.find(name);
    if (it == singletons_.end())
        return false;
    if (it->second.state != SingletonState::Active)
        return false;
    it->second.state = SingletonState::Draining;
    return true;
}

bool SingletonManagerCore::complete_drain(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = singletons_.find(name);
    if (it == singletons_.end())
        return false;
    if (it->second.state != SingletonState::Draining)
        return false;
    it->second.state = SingletonState::Standby;
    return true;
}

size_t SingletonManagerCore::singleton_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return singletons_.size();
}

} // namespace hpactor::cluster::singleton
