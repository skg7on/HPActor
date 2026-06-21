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

// Placeholder — cluster_node_state.hpp will be fully defined in CLU-001 Task 2.
namespace hpactor::cluster {

enum class ClusterNodeState {
    Unknown,
    Alive,
    Suspect,
    Dead,
    Leaving,
    Left,
    Fenced,
};

} // namespace hpactor::cluster
