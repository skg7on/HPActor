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

namespace hpactor::cluster {

/// Cluster node lifecycle state machine.
///
/// Eight states: Joining -> Alive <-> Suspect <-> Unreachable -> Down ->
/// Removed. Quarantined and Leaving are special states: Quarantined requires
/// operator intervention (manual clear back to Joining), and Leaving is a
/// graceful voluntary departure path to Down -> Removed.
enum class ClusterNodeState {
    Joining,     // Node is joining the cluster (initial state)
    Alive,       // Node is healthy and fully participating
    Suspect,     // Node may be failing (gossip-based suspicion)
    Unreachable, // Node is unreachable (network partition or crash)
    Quarantined, // Node is fenced/quarantined (requires operator clear)
    Leaving,     // Node is gracefully leaving the cluster
    Down,        // Node is confirmed down (terminal)
    Removed,     // Node is removed from membership (terminal)
};

/// Returns true if the state represents a live, placement-capable node.
constexpr bool is_alive(ClusterNodeState s) noexcept {
    return s == ClusterNodeState::Alive;
}

/// Returns true if the state is terminal (no further transitions except
/// Down -> Removed, which is the final terminal state).
constexpr bool is_terminal(ClusterNodeState s) noexcept {
    return s == ClusterNodeState::Down || s == ClusterNodeState::Removed;
}

/// Returns true if a transition from `from` to `to` is legal.
/// Self-transitions (from == to) are never allowed.
constexpr bool can_transition(ClusterNodeState from, ClusterNodeState to) noexcept {
    if (from == to) {
        return false;
    }
    switch (from) {
        case ClusterNodeState::Joining:
            return to == ClusterNodeState::Alive;
        case ClusterNodeState::Alive:
            return to == ClusterNodeState::Suspect ||
                   to == ClusterNodeState::Unreachable ||
                   to == ClusterNodeState::Quarantined ||
                   to == ClusterNodeState::Leaving;
        case ClusterNodeState::Suspect:
            return to == ClusterNodeState::Alive ||
                   to == ClusterNodeState::Unreachable ||
                   to == ClusterNodeState::Quarantined;
        case ClusterNodeState::Unreachable:
            return to == ClusterNodeState::Alive || to == ClusterNodeState::Down;
        case ClusterNodeState::Leaving:
            return to == ClusterNodeState::Down;
        case ClusterNodeState::Down:
            return to == ClusterNodeState::Removed;
        case ClusterNodeState::Quarantined:
            return to == ClusterNodeState::Joining;
        case ClusterNodeState::Removed:
            return false;
    }
    return false;
}

/// Returns a snake_case string representation of the state.
constexpr const char* to_string(ClusterNodeState s) noexcept {
    switch (s) {
        case ClusterNodeState::Joining:
            return "joining";
        case ClusterNodeState::Alive:
            return "alive";
        case ClusterNodeState::Suspect:
            return "suspect";
        case ClusterNodeState::Unreachable:
            return "unreachable";
        case ClusterNodeState::Quarantined:
            return "quarantined";
        case ClusterNodeState::Leaving:
            return "leaving";
        case ClusterNodeState::Down:
            return "down";
        case ClusterNodeState::Removed:
            return "removed";
    }
    return "unknown";
}

} // namespace hpactor::cluster
