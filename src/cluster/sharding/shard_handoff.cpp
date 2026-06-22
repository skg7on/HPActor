// Copyright 2026 HPActor Contributors
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

#include <hpactor/cluster/sharding/shard_handoff.hpp>

namespace hpactor::cluster::sharding {

ShardHandoff::ShardHandoff(uint32_t shard_id) : shard_id_(shard_id) {}

uint32_t ShardHandoff::shard_id() const {
    return shard_id_;
}

HandoffState ShardHandoff::state() const {
    return state_;
}

const std::string& ShardHandoff::new_owner() const {
    return new_owner_;
}

bool ShardHandoff::begin_drain() {
    if (state_ != HandoffState::Owned)
        return false;
    state_ = HandoffState::Draining;
    return true;
}

bool ShardHandoff::complete_drain() {
    if (state_ != HandoffState::Draining)
        return false;
    state_ = HandoffState::Transferring;
    return true;
}

bool ShardHandoff::begin_recovery(const std::string& new_owner_node) {
    if (state_ != HandoffState::Transferring)
        return false;
    new_owner_ = new_owner_node;
    state_ = HandoffState::Recovering;
    return true;
}

bool ShardHandoff::activate() {
    if (state_ != HandoffState::Recovering)
        return false;
    state_ = HandoffState::Active;
    return true;
}

bool ShardHandoff::abort() {
    if (state_ != HandoffState::Draining)
        return false;
    state_ = HandoffState::Owned;
    return true;
}

} // namespace hpactor::cluster::sharding
