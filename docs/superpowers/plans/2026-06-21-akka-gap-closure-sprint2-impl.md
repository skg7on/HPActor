# Sprint 2 Cluster Abstractions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement CLU-001 Cluster Failure Model & Fencing — the foundational control-plane subsystem that all subsequent cluster abstractions depend on.

**Architecture:** Eight-state node state machine with explicit transitions fed by GossipMembership events. Fencing via incarnation/epoch/cluster-id identity validation. Partition policy (FailOpen/FailClosed/StaticMajority). Route invalidation on node state change (cache purge, pending-op failure, connection close).

**Tech Stack:** C++20, Google Test, Ninja build, SchedulerTestDriver for deterministic tests.

**Design spec:** `docs/superpowers/specs/2026-06-21-akka-gap-closure-sprint2-design.md`

**Note:** This plan covers PR #1 (CLU-001) only. PRs #2-4 (CLU-002, CLU-003, MSG-003) receive separate plans after PR #1 lands.

---

## File Map

### New files created

```
include/hpactor/cluster/
├── cluster_node_state.hpp         — ClusterNodeState enum, transition table, predicates
├── cluster_node_identity.hpp      — ClusterNodeIdentity struct, fencing helpers
├── cluster_failure_model.hpp      — ClusterFailureModel class (policy engine)
├── partition_policy.hpp           — PartitionPolicy enum, quorum helpers
└── route_invalidation.hpp         — RouteInvalidation orchestration class

src/cluster/
├── CMakeLists.txt                 — Build target for hpactor_cluster
├── cluster_failure_model.cpp      — State machine, fencing, invalidation dispatch
├── partition_policy.cpp           — Policy decision logic
└── route_invalidation.cpp         — Cache purge + op-failure orchestration

tests/unit/cluster/
├── CMakeLists.txt                 — Test build target
├── test_cluster_node_state.cpp    — State transitions, illegal transition rejection
├── test_cluster_fencing.cpp       — Incarnation fencing, duplicate identity
├── test_cluster_route_invalidation.cpp — Cache purge, pending-op failure
└── test_partition_policy.cpp      — Policy decisions per type
```

### Existing files modified

```
include/hpactor/msg/failure_reason.hpp   — Add NodeQuarantined=3, NodeReplaced=4 (route range)
include/hpactor/net/actor_location_cache.hpp — Add purge_node(NodeId) method
src/net/actor_location_cache.cpp              — Purge implementation
include/hpactor/net/connection_pool.hpp       — Add can_connect() check
include/hpactor/core/config.hpp               — Add ClusterConfig fields (cluster_id, partition_policy)
tests/support/system_test_fixture.hpp         — Add cluster_mode_config() helper
src/CMakeLists.txt                            — Add subdirectory for src/cluster
tests/unit/CMakeLists.txt                     — Add subdirectory for tests/unit/cluster
```

---

## Task 1: Build Infrastructure — CMake + Directory Scaffold

**Files:**
- Create: `src/cluster/CMakeLists.txt`
- Create: `tests/unit/cluster/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Create `src/cluster/CMakeLists.txt`**

```cmake
add_library(hpactor_cluster STATIC
    cluster_failure_model.cpp
    partition_policy.cpp
    route_invalidation.cpp
)
target_link_libraries(hpactor_cluster PUBLIC hpactor_core)
target_include_directories(hpactor_cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
```

- [ ] **Step 2: Create `tests/unit/cluster/CMakeLists.txt`**

```cmake
add_executable(test_unit_cluster
    test_cluster_node_state.cpp
    test_cluster_fencing.cpp
    test_cluster_route_invalidation.cpp
    test_partition_policy.cpp
)
target_link_libraries(test_unit_cluster hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_cluster)
```

- [ ] **Step 3: Add `add_subdirectory(cluster)` to `src/CMakeLists.txt`**

Run: `grep -n "add_subdirectory" src/CMakeLists.txt` to find insertion point.
Insert after last existing `add_subdirectory`:
```cmake
add_subdirectory(cluster)
```

- [ ] **Step 4: Add `add_subdirectory(cluster)` to `tests/unit/CMakeLists.txt`**

Run: `grep -n "add_subdirectory" tests/unit/CMakeLists.txt` to find insertion point.
Insert after last existing `add_subdirectory`:
```cmake
add_subdirectory(cluster)
```

- [ ] **Step 5: Create empty placeholder .cpp files so the build doesn't fail**

```bash
touch src/cluster/cluster_failure_model.cpp
touch src/cluster/partition_policy.cpp
touch src/cluster/route_invalidation.cpp
```

Each file must contain a minimal include to compile:
```cpp
// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#include <hpactor/cluster/cluster_node_state.hpp>
```

- [ ] **Step 6: Configure and build to verify scaffold compiles**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/akka-gap-closure-sprint2
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_APPS=OFF -DENABLE_EXAMPLES=OFF
ninja -C build hpactor_cluster
```

Expected: Build succeeds (empty library compiles).

- [ ] **Step 7: Commit scaffold**

```bash
git add src/cluster/ tests/unit/cluster/ src/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "build: add cluster subsystem CMake scaffold

Prepares directory structure for CLU-001 cluster failure model.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: ClusterNodeState — Enum, Transition Table, Predicates (RED)

**Files:**
- Create: `include/hpactor/cluster/cluster_node_state.hpp`
- Create: `tests/unit/cluster/test_cluster_node_state.cpp`

- [ ] **Step 1: Write the failing test — `tests/unit/cluster/test_cluster_node_state.cpp`**

```cpp
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

#include <gtest/gtest.h>
#include <hpactor/cluster/cluster_node_state.hpp>

namespace hpactor::cluster {

TEST(ClusterNodeStateTest, AllStatesAreDefined) {
    // Verify every state enum value exists and has a distinct integer
    EXPECT_NE(static_cast<uint8_t>(ClusterNodeState::Joining),
              static_cast<uint8_t>(ClusterNodeState::Alive));
    EXPECT_NE(static_cast<uint8_t>(ClusterNodeState::Alive),
              static_cast<uint8_t>(ClusterNodeState::Suspect));
}

TEST(ClusterNodeStateTest, AliveAcceptsPlacement) {
    EXPECT_TRUE(is_alive(ClusterNodeState::Alive));
    EXPECT_FALSE(is_alive(ClusterNodeState::Suspect));
    EXPECT_FALSE(is_alive(ClusterNodeState::Unreachable));
    EXPECT_FALSE(is_alive(ClusterNodeState::Down));
    EXPECT_FALSE(is_alive(ClusterNodeState::Quarantined));
    EXPECT_FALSE(is_alive(ClusterNodeState::Joining));
    EXPECT_FALSE(is_alive(ClusterNodeState::Leaving));
    EXPECT_FALSE(is_alive(ClusterNodeState::Removed));
}

TEST(ClusterNodeStateTest, DownAndRemovedAreTerminal) {
    EXPECT_TRUE(is_terminal(ClusterNodeState::Down));
    EXPECT_TRUE(is_terminal(ClusterNodeState::Removed));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Alive));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Suspect));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Quarantined));
}

TEST(ClusterNodeStateTest, QuarantinedNoAutoRecovery) {
    // Quarantined is not terminal but also not alive — routes are invalid
    EXPECT_FALSE(is_alive(ClusterNodeState::Quarantined));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Quarantined));
}

TEST(ClusterNodeStateTest, TransitionJoiningToAlive) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Joining,
                               ClusterNodeState::Alive));
}

TEST(ClusterNodeStateTest, TransitionAliveToSuspect) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Alive,
                               ClusterNodeState::Suspect));
}

TEST(ClusterNodeStateTest, TransitionSuspectBackToAlive) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Suspect,
                               ClusterNodeState::Alive));
}

TEST(ClusterNodeStateTest, TransitionSuspectToUnreachable) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Suspect,
                               ClusterNodeState::Unreachable));
}

TEST(ClusterNodeStateTest, TransitionAliveToUnreachable) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Alive,
                               ClusterNodeState::Unreachable));
}

TEST(ClusterNodeStateTest, TransitionUnreachableToAlive) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Unreachable,
                               ClusterNodeState::Alive));
}

TEST(ClusterNodeStateTest, TransitionUnreachableToDown) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Unreachable,
                               ClusterNodeState::Down));
}

TEST(ClusterNodeStateTest, TransitionAliveToQuarantined) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Alive,
                               ClusterNodeState::Quarantined));
}

TEST(ClusterNodeStateTest, TransitionSuspectToQuarantined) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Suspect,
                               ClusterNodeState::Quarantined));
}

TEST(ClusterNodeStateTest, TransitionAliveToLeaving) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Alive,
                               ClusterNodeState::Leaving));
}

TEST(ClusterNodeStateTest, TransitionLeavingToDown) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Leaving,
                               ClusterNodeState::Down));
}

TEST(ClusterNodeStateTest, TransitionDownToRemoved) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Down,
                               ClusterNodeState::Removed));
}

TEST(ClusterNodeStateTest, QuarantinedToJoiningViaOperator) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Quarantined,
                               ClusterNodeState::Joining));
}

TEST(ClusterNodeStateTest, NoTransitionFromDownToAlive) {
    // Down is terminal for this incarnation — cannot go back to Alive
    EXPECT_FALSE(can_transition(ClusterNodeState::Down,
                                ClusterNodeState::Alive));
}

TEST(ClusterNodeStateTest, NoTransitionFromRemoved) {
    // Removed nodes cannot transition to anything else
    EXPECT_FALSE(can_transition(ClusterNodeState::Removed,
                                ClusterNodeState::Joining));
    EXPECT_FALSE(can_transition(ClusterNodeState::Removed,
                                ClusterNodeState::Alive));
}

TEST(ClusterNodeStateTest, NoTransitionFromQuarantinedToAlive) {
    // Quarantined cannot go directly to Alive — must go through Joining
    EXPECT_FALSE(can_transition(ClusterNodeState::Quarantined,
                                ClusterNodeState::Alive));
}

TEST(ClusterNodeStateTest, NoSelfTransition) {
    // A state should never "transition" to itself
    EXPECT_FALSE(can_transition(ClusterNodeState::Alive,
                                ClusterNodeState::Alive));
    EXPECT_FALSE(can_transition(ClusterNodeState::Down,
                                ClusterNodeState::Down));
}

TEST(ClusterNodeStateTest, ToStringReturnsNonNull) {
    EXPECT_NE(to_string(ClusterNodeState::Alive), nullptr);
    EXPECT_NE(to_string(ClusterNodeState::Down), nullptr);
    EXPECT_NE(to_string(ClusterNodeState::Quarantined), nullptr);
    EXPECT_STREQ(to_string(ClusterNodeState::Alive), "alive");
    EXPECT_STREQ(to_string(ClusterNodeState::Down), "down");
    EXPECT_STREQ(to_string(ClusterNodeState::Quarantined), "quarantined");
}

} // namespace hpactor::cluster
```

- [ ] **Step 2: Run test to verify it fails (compilation error)**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/akka-gap-closure-sprint2
ninja -C build test_unit_cluster 2>&1 | tail -20
```

Expected: Compilation fails — `ClusterNodeState` not defined, `is_alive`, `is_terminal`, `can_transition`, `to_string` not found.

---

## Task 3: ClusterNodeState — Header Implementation (GREEN)

**File:**
- Create: `include/hpactor/cluster/cluster_node_state.hpp`

- [ ] **Step 1: Write the header**

```cpp
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

namespace hpactor::cluster {

/// \brief Cluster node state — the authoritative lifecycle of a node from
///        a peer's perspective.
///
/// Only \c Alive accepts new actor placement and user traffic. All other
/// states gate or reject routing.
enum class ClusterNodeState : uint8_t {
    Joining,      ///< New node discovered, not yet trusted for placement.
    Alive,        ///< Node can receive new work and actor placement.
    Suspect,      ///< Failure detector is probing; temporarily distrusted.
    Unreachable,  ///< Cannot be contacted from this node's perspective.
    Quarantined,  ///< Identity/epoch conflict makes communication unsafe.
    Leaving,      ///< Graceful shutdown has begun.
    Down,         ///< Considered failed for routing and placement.
    Removed,      ///< No longer part of cluster metadata; tombstone expired.
};

/// \brief Whether this state accepts actor placement and user traffic.
///
/// \param[in] state The node state to test.
/// \return true iff state is \c Alive.
constexpr bool is_alive(ClusterNodeState state) noexcept {
    return state == ClusterNodeState::Alive;
}

/// \brief Whether this state is terminal for the current incarnation.
///
/// Terminal states cannot transition back to \c Alive or \c Suspect.
///
/// \param[in] state The node state to test.
/// \return true iff state is \c Down or \c Removed.
constexpr bool is_terminal(ClusterNodeState state) noexcept {
    return state == ClusterNodeState::Down ||
           state == ClusterNodeState::Removed;
}

/// \brief Whether a transition from \p from to \p to is legal.
///
/// Transitions are monotonic for a given incarnation. Self-transitions
/// are not permitted. Quarantined nodes may only transition to Joining
/// (via operator intervention).
///
/// \param[in] from The current state.
/// \param[in] to   The proposed new state.
/// \return true iff the transition is legal.
constexpr bool can_transition(ClusterNodeState from,
                              ClusterNodeState to) noexcept {
    if (from == to) return false;
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
        return to == ClusterNodeState::Alive ||
               to == ClusterNodeState::Down;
    case ClusterNodeState::Quarantined:
        return to == ClusterNodeState::Joining;
    case ClusterNodeState::Leaving:
        return to == ClusterNodeState::Down;
    case ClusterNodeState::Down:
        return to == ClusterNodeState::Removed;
    case ClusterNodeState::Removed:
        return false;
    }
    return false;
}

/// \brief Human-readable snake_case string for the node state.
///
/// \param[in] state The node state.
/// \return A null-terminated snake_case string literal (e.g. "alive",
///         "quarantined"). Never returns nullptr.
constexpr const char* to_string(ClusterNodeState state) noexcept {
    switch (state) {
    case ClusterNodeState::Joining:     return "joining";
    case ClusterNodeState::Alive:       return "alive";
    case ClusterNodeState::Suspect:     return "suspect";
    case ClusterNodeState::Unreachable: return "unreachable";
    case ClusterNodeState::Quarantined: return "quarantined";
    case ClusterNodeState::Leaving:     return "leaving";
    case ClusterNodeState::Down:        return "down";
    case ClusterNodeState::Removed:     return "removed";
    }
    return "unknown";
}

} // namespace hpactor::cluster
```

- [ ] **Step 2: Update `src/cluster/cluster_failure_model.cpp` placeholder**

```cpp
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

#include <hpactor/cluster/cluster_failure_model.hpp>

namespace hpactor::cluster {

// Implementation will be added in subsequent tasks.

} // namespace hpactor::cluster
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="ClusterNodeStateTest*"
```

Expected: All 21 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cluster/cluster_node_state.hpp src/cluster/cluster_failure_model.cpp tests/unit/cluster/test_cluster_node_state.cpp
git commit -m "feat(cluster): add ClusterNodeState enum with transition table and predicates

Eight-state model: Joining, Alive, Suspect, Unreachable, Quarantined,
Leaving, Down, Removed. Constexpr transition table with is_alive(),
is_terminal(), and can_transition() predicates. 21 tests covering
all legal transitions, terminal checks, and illegal transition rejection.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 4: ClusterNodeIdentity — Identity Struct + Fencing (RED)

**Files:**
- Create: `include/hpactor/cluster/cluster_node_identity.hpp`
- Create: `tests/unit/cluster/test_cluster_fencing.cpp`

- [ ] **Step 1: Write the failing test — `tests/unit/cluster/test_cluster_fencing.cpp`**

```cpp
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

#include <gtest/gtest.h>
#include <hpactor/cluster/cluster_node_identity.hpp>

namespace hpactor::cluster {

TEST(ClusterNodeIdentityTest, DefaultConstruction) {
    ClusterNodeIdentity id;
    EXPECT_EQ(id.incarnation, 0);
    EXPECT_EQ(id.process_start_id, 0);
    EXPECT_EQ(id.membership_epoch, 0);
}

TEST(ClusterNodeIdentityTest, HigherIncarnationFencesOlder) {
    ClusterNodeIdentity old_id;
    old_id.node_id = NodeId("node-1");
    old_id.incarnation = 1;
    old_id.process_start_id = 100;

    ClusterNodeIdentity new_id;
    new_id.node_id = NodeId("node-1");
    new_id.incarnation = 2;
    new_id.process_start_id = 200;

    // Newer incarnation should fence the older one
    EXPECT_TRUE(fences(new_id, old_id));
    // Older cannot fence newer
    EXPECT_FALSE(fences(old_id, new_id));
}

TEST(ClusterNodeIdentityTest, DifferentNodesCannotFence) {
    ClusterNodeIdentity node_a;
    node_a.node_id = NodeId("node-a");
    node_a.incarnation = 5;

    ClusterNodeIdentity node_b;
    node_b.node_id = NodeId("node-b");
    node_b.incarnation = 1;

    // Different node ids — fencing is irrelevant
    EXPECT_FALSE(fences(node_a, node_b));
    EXPECT_FALSE(fences(node_b, node_a));
}

TEST(ClusterNodeIdentityTest, SameIncarnationDifferentProcessStartIsConflict) {
    ClusterNodeIdentity a;
    a.node_id = NodeId("node-1");
    a.incarnation = 3;
    a.process_start_id = 100;

    ClusterNodeIdentity b;
    b.node_id = NodeId("node-1");
    b.incarnation = 3;
    b.process_start_id = 200;

    EXPECT_TRUE(is_identity_conflict(a, b));
}

TEST(ClusterNodeIdentityTest, SameIdentityNoConflict) {
    ClusterNodeIdentity a;
    a.node_id = NodeId("node-1");
    a.incarnation = 3;
    a.process_start_id = 100;

    ClusterNodeIdentity b = a; // identical

    EXPECT_FALSE(is_identity_conflict(a, b));
}

TEST(ClusterNodeIdentityTest, DifferentClusterIdRejected) {
    ClusterNodeIdentity local;
    local.node_id = NodeId("node-1");
    local.cluster_id = ClusterId("prod-us-east");

    ClusterNodeIdentity remote;
    remote.node_id = NodeId("node-2");
    remote.cluster_id = ClusterId("prod-us-west");

    EXPECT_FALSE(same_cluster(local, remote));
}

TEST(ClusterNodeIdentityTest, SameClusterIdAccepted) {
    ClusterNodeIdentity local;
    local.node_id = NodeId("node-1");
    local.cluster_id = ClusterId("prod-us-east");

    ClusterNodeIdentity remote;
    remote.node_id = NodeId("node-2");
    remote.cluster_id = ClusterId("prod-us-east");

    EXPECT_TRUE(same_cluster(local, remote));
}

TEST(ClusterNodeIdentityTest, StaleEpochDetected) {
    ClusterNodeIdentity current;
    current.membership_epoch = 10;

    ClusterNodeIdentity stale;
    stale.membership_epoch = 5;

    EXPECT_TRUE(has_stale_epoch(stale, current));
    EXPECT_FALSE(has_stale_epoch(current, stale));
    EXPECT_FALSE(has_stale_epoch(current, current));
}

} // namespace hpactor::cluster
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_cluster 2>&1 | tail -20
```

Expected: Compilation fails — `ClusterNodeIdentity`, `ClusterId`, `fences()`, `is_identity_conflict()`, `same_cluster()`, `has_stale_epoch()` not defined.

---

## Task 5: ClusterNodeIdentity — Header Implementation (GREEN)

**File:**
- Create: `include/hpactor/cluster/cluster_node_identity.hpp`

- [ ] **Step 1: Write the header**

```cpp
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

namespace hpactor::cluster {

/// \brief Logical cluster namespace. Nodes with different cluster IDs
///        reject each other at the transport handshake.
using ClusterId = std::string;

/// \brief Node identity fields carried on every transport connection and
///        membership advertisement.
///
/// These fields enable fencing (higher incarnation wins), identity conflict
/// detection (same incarnation, different process), cluster-id gating, and
/// epoch-based membership validation.
struct ClusterNodeIdentity {
    std::string node_id;         ///< Stable logical node name.
    uint64_t incarnation = 0;    ///< Monotonic per-start counter; higher wins.
    uint64_t process_start_id
        = 0;                    ///< Boot-id or monotonic counter per process
                                 ///< start.
    uint64_t membership_epoch
        = 0;                    ///< Cluster membership generation; stale ones
                                 ///< must re-join.
    ClusterId cluster_id;        ///< Cluster namespace.
};

/// \brief Whether \p newer fences \p older — i.e. same node, higher
///        incarnation.
///
/// \param[in] newer The candidate newer identity.
/// \param[in] older The candidate older identity.
/// \return true if \p newer represents the same node with a higher incarnation.
constexpr bool fences(const ClusterNodeIdentity& newer,
                      const ClusterNodeIdentity& older) noexcept {
    return newer.node_id == older.node_id &&
           newer.incarnation > older.incarnation;
}

/// \brief Whether two identities represent a conflict — same node, same
///        incarnation, but different process start IDs. Both should be
///        quarantined.
///
/// \param[in] a First identity.
/// \param[in] b Second identity.
/// \return true if the identities conflict.
constexpr bool is_identity_conflict(
    const ClusterNodeIdentity& a,
    const ClusterNodeIdentity& b) noexcept {
    return a.node_id == b.node_id &&
           a.incarnation == b.incarnation &&
           a.process_start_id != b.process_start_id;
}

/// \brief Whether two identities belong to the same cluster namespace.
///
/// \param[in] a First identity.
/// \param[in] b Second identity.
/// \return true if both have the same cluster_id.
inline bool same_cluster(const ClusterNodeIdentity& a,
                         const ClusterNodeIdentity& b) {
    return a.cluster_id == b.cluster_id;
}

/// \brief Whether \p candidate has a membership epoch older than \p current.
///
/// \param[in] candidate The identity being evaluated.
/// \param[in] current   The current membership epoch reference.
/// \return true if the candidate's epoch is lower.
constexpr bool has_stale_epoch(
    const ClusterNodeIdentity& candidate,
    const ClusterNodeIdentity& current) noexcept {
    return candidate.membership_epoch < current.membership_epoch;
}

} // namespace hpactor::cluster
```

- [ ] **Step 2: Build and run tests**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="ClusterNodeIdentityTest*"
```

Expected: All 8 tests pass.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cluster/cluster_node_identity.hpp tests/unit/cluster/test_cluster_fencing.cpp
git commit -m "feat(cluster): add ClusterNodeIdentity with fencing predicates

Identity struct carries node_id, incarnation, process_start_id,
membership_epoch, and cluster_id. Constexpr fencing: fences() for
higher-incarnation detection, is_identity_conflict() for duplicate
process detection, same_cluster() for cluster-id gating, and
has_stale_epoch() for membership validation.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 6: PartitionPolicy — Enum + Decision Helpers (RED)

**Files:**
- Create: `include/hpactor/cluster/partition_policy.hpp`
- Create: `tests/unit/cluster/test_partition_policy.cpp`

- [ ] **Step 1: Write the failing test — `tests/unit/cluster/test_partition_policy.cpp`**

```cpp
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

#include <gtest/gtest.h>
#include <hpactor/cluster/partition_policy.hpp>

namespace hpactor::cluster {

TEST(PartitionPolicyTest, AllPoliciesDefined) {
    EXPECT_NE(static_cast<uint8_t>(PartitionPolicy::FailOpen),
              static_cast<uint8_t>(PartitionPolicy::FailClosed));
    EXPECT_NE(static_cast<uint8_t>(PartitionPolicy::FailClosed),
              static_cast<uint8_t>(PartitionPolicy::StaticMajority));
}

TEST(PartitionPolicyTest, FailOpenAllowsDelivery) {
    EXPECT_TRUE(allow_user_delivery(PartitionPolicy::FailOpen, /*quorum=*/false));
    EXPECT_TRUE(allow_user_delivery(PartitionPolicy::FailOpen, /*quorum=*/true));
}

TEST(PartitionPolicyTest, FailClosedBlocksWithoutQuorum) {
    EXPECT_FALSE(allow_user_delivery(PartitionPolicy::FailClosed, /*quorum=*/false));
    EXPECT_TRUE(allow_user_delivery(PartitionPolicy::FailClosed, /*quorum=*/true));
}

TEST(PartitionPolicyTest, FailClosedBlocksSingletonOwnershipWithoutQuorum) {
    EXPECT_FALSE(allow_ownership_change(PartitionPolicy::FailClosed, /*quorum=*/false));
}

TEST(PartitionPolicyTest, FailOpenAllowsOwnershipWithQuorum) {
    EXPECT_TRUE(allow_ownership_change(PartitionPolicy::FailOpen, /*quorum=*/true));
}

TEST(PartitionPolicyTest, StaticMajorityRequiresConfiguredMajority) {
    // With majority present, operations are allowed
    EXPECT_TRUE(allow_ownership_change(PartitionPolicy::StaticMajority, /*majority=*/true));
    // Without majority, blocked
    EXPECT_FALSE(allow_ownership_change(PartitionPolicy::StaticMajority, /*majority=*/false));
}

TEST(PartitionPolicyTest, ToStringReturnsNonNull) {
    EXPECT_NE(to_string(PartitionPolicy::FailOpen), nullptr);
    EXPECT_NE(to_string(PartitionPolicy::FailClosed), nullptr);
    EXPECT_NE(to_string(PartitionPolicy::StaticMajority), nullptr);
    EXPECT_STREQ(to_string(PartitionPolicy::FailOpen), "fail_open");
    EXPECT_STREQ(to_string(PartitionPolicy::FailClosed), "fail_closed");
    EXPECT_STREQ(to_string(PartitionPolicy::StaticMajority), "static_majority");
}

} // namespace hpactor::cluster
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_cluster 2>&1 | tail -10
```

Expected: Compilation fails — `PartitionPolicy`, `allow_user_delivery()`, `allow_ownership_change()`, `to_string()` not found.

---

## Task 7: PartitionPolicy — Header Implementation (GREEN)

**File:**
- Create: `include/hpactor/cluster/partition_policy.hpp`

- [ ] **Step 1: Write the header**

```cpp
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

namespace hpactor::cluster {

/// \brief Partition behavior policy — controls delivery and ownership
///        decisions when the cluster is partitioned.
///
/// Default for user messages: \c FailOpen.
/// Default for singleton/shard ownership: \c FailClosed.
enum class PartitionPolicy : uint8_t {
    FailOpen,       ///< Continue best-effort delivery to reachable nodes.
    FailClosed,     ///< Stop remote delivery unless quorum is known.
    StaticMajority, ///< Only configured majority partition owns coordinator
                    ///< roles.
};

/// \brief Whether user messages may be delivered to remote nodes under
///        this policy given the current quorum state.
///
/// \param[in] policy The partition policy.
/// \param[in] quorum_known Whether a quorum of expected nodes is reachable.
/// \return true if user delivery should proceed.
constexpr bool allow_user_delivery(PartitionPolicy policy,
                                   bool quorum_known) noexcept {
    switch (policy) {
    case PartitionPolicy::FailOpen:
        return true; // Always allow best-effort delivery
    case PartitionPolicy::FailClosed:
        return quorum_known;
    case PartitionPolicy::StaticMajority:
        return quorum_known; // Same as FailClosed for user messages
    }
    return false;
}

/// \brief Whether singleton/shard ownership changes are permitted under
///        this policy.
///
/// \param[in] policy The partition policy.
/// \param[in] majority_present Whether a configured majority of nodes is
///                             reachable.
/// \return true if ownership changes are allowed.
constexpr bool allow_ownership_change(PartitionPolicy policy,
                                      bool majority_present) noexcept {
    switch (policy) {
    case PartitionPolicy::FailOpen:
        return majority_present;
    case PartitionPolicy::FailClosed:
        return majority_present;
    case PartitionPolicy::StaticMajority:
        return majority_present;
    }
    return false;
}

/// \brief Human-readable snake_case string for the partition policy.
///
/// \param[in] policy The partition policy.
/// \return A null-terminated snake_case string literal.
constexpr const char* to_string(PartitionPolicy policy) noexcept {
    switch (policy) {
    case PartitionPolicy::FailOpen:       return "fail_open";
    case PartitionPolicy::FailClosed:     return "fail_closed";
    case PartitionPolicy::StaticMajority: return "static_majority";
    }
    return "unknown";
}

} // namespace hpactor::cluster
```

- [ ] **Step 2: Build and run tests**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="PartitionPolicyTest*"
```

Expected: All 7 tests pass.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cluster/partition_policy.hpp tests/unit/cluster/test_partition_policy.cpp
git commit -m "feat(cluster): add PartitionPolicy enum with decision helpers

Three policies: FailOpen (best-effort always), FailClosed (require quorum),
StaticMajority (require configured majority). Constexpr allow_user_delivery()
and allow_ownership_change() helpers. to_string() for observability.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 8: ClusterFailureModel — Policy Engine (RED)

**Files:**
- Create: `include/hpactor/cluster/cluster_failure_model.hpp`
- Create: `src/cluster/cluster_failure_model.cpp` (full impl)
- Create: `tests/unit/cluster/test_cluster_route_invalidation.cpp`

- [ ] **Step 1: Write the failing test — `tests/unit/cluster/test_cluster_route_invalidation.cpp`**

```cpp
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

#include <gtest/gtest.h>
#include <hpactor/cluster/cluster_failure_model.hpp>

namespace hpactor::cluster {

class ClusterFailureModelTest : public ::testing::Test {
protected:
    void SetUp() override {
        model_ = ClusterFailureModel();
    }

    ClusterFailureModel model_;
};

TEST_F(ClusterFailureModelTest, NewModelHasEmptyNodeMap) {
    EXPECT_EQ(model_.node_count(), 0);
}

TEST_F(ClusterFailureModelTest, RegisterNodeAddsToMap) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.incarnation = 1;
    id.process_start_id = 100;
    id.cluster_id = "test-cluster";

    EXPECT_TRUE(model_.register_node(id));
    EXPECT_EQ(model_.node_count(), 1);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Joining);
}

TEST_F(ClusterFailureModelTest, TransitionNodeChangesState) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    // Joining → Alive
    auto result = model_.transition("node-1", ClusterNodeState::Alive,
                                    "handshake complete");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Alive);
}

TEST_F(ClusterFailureModelTest, TransitionRejectsIllegalMove) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    model_.transition("node-1", ClusterNodeState::Alive, "handshake complete");

    // Alive → Removed is illegal (must go through Down first)
    auto result = model_.transition("node-1", ClusterNodeState::Removed,
                                    "bad transition");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Alive); // unchanged
}

TEST_F(ClusterFailureModelTest, TransitionToDownTriggersInvalidation) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    model_.transition("node-1", ClusterNodeState::Alive, "ready");

    // Transition Alive → Unreachable → Down
    model_.transition("node-1", ClusterNodeState::Unreachable, "partition");
    auto result = model_.transition("node-1", ClusterNodeState::Down,
                                    "prolonged unreachable");

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.routes_invalidated);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Down);
}

TEST_F(ClusterFailureModelTest, RegisterDuplicateIdentityQuarantines) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.incarnation = 1;
    id.process_start_id = 100;
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    model_.transition("node-1", ClusterNodeState::Alive, "ready");

    // Same incarnation, different process_start_id
    ClusterNodeIdentity duplicate;
    duplicate.node_id = "node-1";
    duplicate.incarnation = 1;
    duplicate.process_start_id = 200;  // different process
    duplicate.cluster_id = "test-cluster";

    auto status = model_.check_identity_conflict(duplicate);
    EXPECT_TRUE(status.conflict_detected);
    EXPECT_EQ(status.resolution, IdentityConflictResolution::QuarantineBoth);
}

TEST_F(ClusterFailureModelTest, HigherIncarnationFencesOld) {
    ClusterNodeIdentity id_v1;
    id_v1.node_id = "node-1";
    id_v1.incarnation = 1;
    id_v1.process_start_id = 100;
    id_v1.cluster_id = "test-cluster";

    model_.register_node(id_v1);
    model_.transition("node-1", ClusterNodeState::Alive, "ready");

    ClusterNodeIdentity id_v2;
    id_v2.node_id = "node-1";
    id_v2.incarnation = 2;  // higher
    id_v2.process_start_id = 200;
    id_v2.cluster_id = "test-cluster";

    auto status = model_.check_identity_conflict(id_v2);
    EXPECT_FALSE(status.conflict_detected);
    EXPECT_TRUE(status.fence_old);
}

TEST_F(ClusterFailureModelTest, UnknownNodeReturnsSentinel) {
    EXPECT_EQ(model_.get_state("nonexistent"), ClusterNodeState::Removed);
}

TEST_F(ClusterFailureModelTest, TransitionOnUnknownNodeFails) {
    auto result = model_.transition("nonexistent", ClusterNodeState::Alive,
                                    "test");
    EXPECT_FALSE(result.success);
}

TEST_F(ClusterFailureModelTest, DefaultPolicyIsFailOpen) {
    EXPECT_EQ(model_.get_partition_policy(), PartitionPolicy::FailOpen);
}

TEST_F(ClusterFailureModelTest, SetPartitionPolicy) {
    model_.set_partition_policy(PartitionPolicy::FailClosed);
    EXPECT_EQ(model_.get_partition_policy(), PartitionPolicy::FailClosed);
}

} // namespace hpactor::cluster
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_cluster 2>&1 | tail -10
```

Expected: Compilation fails — `ClusterFailureModel`, `TransitionResult`, `IdentityConflictResolution`, `IdentityCheckStatus` not defined.

---

## Task 9: ClusterFailureModel — Full Implementation (GREEN)

**Files:**
- Overwrite: `include/hpactor/cluster/cluster_failure_model.hpp`
- Overwrite: `src/cluster/cluster_failure_model.cpp`

- [ ] **Step 1: Write the header — `include/hpactor/cluster/cluster_failure_model.hpp`**

```cpp
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

#include <hpactor/cluster/cluster_node_identity.hpp>
#include <hpactor/cluster/cluster_node_state.hpp>
#include <hpactor/cluster/partition_policy.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::cluster {

/// \brief Result of a state transition request.
struct TransitionResult {
    bool success = false;            ///< Whether the transition was allowed.
    bool routes_invalidated = false; ///< Whether routes were invalidated as a
                                     ///  side effect (Down/Quarantined/Removed).
    std::string reason;              ///< Human-readable reason if rejected.
};

/// \brief Resolution for an identity conflict.
enum class IdentityConflictResolution : uint8_t {
    None,             ///< No conflict.
    QuarantineBoth,   ///< Both identities should be quarantined.
    FenceOld,         ///< Old identity is fenced; new one accepted.
};

/// \brief Result of an identity conflict check.
struct IdentityCheckStatus {
    bool conflict_detected = false;
    IdentityConflictResolution resolution = IdentityConflictResolution::None;
};

/// \brief Manages cluster node state, fencing, and route invalidation.
///
/// Owns the authoritative node state map. Receives membership events from
/// GossipMembership and applies policy decisions. Coordinates route
/// invalidation when nodes enter Down, Quarantined, or Removed states.
///
/// \note Thread safety: All public methods are safe to call from any thread.
///       Internal state is protected by a mutex.
class ClusterFailureModel {
public:
    ClusterFailureModel();
    ~ClusterFailureModel();

    // ── Node registration & lifecycle ──────────────────────────

    /// \brief Register a new node identity. Node starts in \c Joining state.
    ///
    /// \param[in] id The node identity.
    /// \return true if registration succeeded (node not already registered).
    bool register_node(const ClusterNodeIdentity& id);

    /// \brief Attempt to transition a node to a new state.
    ///
    /// \param[in] node_id The node to transition.
    /// \param[in] to       The target state.
    /// \param[in] reason   Human-readable reason (logged).
    /// \return \c TransitionResult describing success or rejection.
    TransitionResult transition(const std::string& node_id,
                                ClusterNodeState to,
                                const std::string& reason);

    /// \brief Get the current state of a node.
    ///
    /// \param[in] node_id The node to query.
    /// \return The current state, or \c Removed if the node is unknown.
    ClusterNodeState get_state(const std::string& node_id) const;

    /// \brief Total count of registered nodes.
    size_t node_count() const;

    // ── Identity & fencing ─────────────────────────────────────

    /// \brief Check a received identity for conflicts with an existing
    ///        registered node.
    ///
    /// \param[in] received The identity received from the network.
    /// \return An \c IdentityCheckStatus with conflict detection and
    ///         recommended resolution.
    IdentityCheckStatus
    check_identity_conflict(const ClusterNodeIdentity& received) const;

    // ── Partition policy ───────────────────────────────────────

    /// \brief Get the current partition policy.
    PartitionPolicy get_partition_policy() const;

    /// \brief Set the partition policy.
    void set_partition_policy(PartitionPolicy policy);

    // ── Quorum ─────────────────────────────────────────────────

    /// \brief Whether a quorum of expected nodes is currently reachable.
    ///
    /// A quorum is defined as more than half of all known nodes in Alive
    /// state.
    bool quorum_present() const;

    // ── Bulk queries ───────────────────────────────────────────

    /// \brief Get all currently Alive node IDs.
    std::vector<std::string> alive_nodes() const;

    /// \brief Get the set of nodes that transitioned since last call.
    ///
    /// Used by route invalidation to discover nodes needing cache purge.
    std::vector<std::string> drain_invalidation_queue();

private:
    struct NodeRecord {
        ClusterNodeIdentity identity;
        ClusterNodeState state = ClusterNodeState::Joining;
    };

    std::unordered_map<std::string, NodeRecord> nodes_;
    PartitionPolicy partition_policy_ = PartitionPolicy::FailOpen;
    std::vector<std::string> invalidation_queue_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::cluster
```

- [ ] **Step 2: Write the implementation — `src/cluster/cluster_failure_model.cpp`**

```cpp
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

#include <hpactor/cluster/cluster_failure_model.hpp>

#include <algorithm>
#include <mutex>

namespace hpactor::cluster {

ClusterFailureModel::ClusterFailureModel() = default;
ClusterFailureModel::~ClusterFailureModel() = default;

bool ClusterFailureModel::register_node(const ClusterNodeIdentity& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.find(id.node_id) != nodes_.end()) {
        return false; // Already registered
    }
    nodes_[id.node_id] = NodeRecord{id, ClusterNodeState::Joining};
    return true;
}

TransitionResult
ClusterFailureModel::transition(const std::string& node_id,
                                ClusterNodeState to,
                                const std::string& /*reason*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return {false, false, "node not found"};
    }

    ClusterNodeState from = it->second.state;

    if (!can_transition(from, to)) {
        return {false, false, "illegal transition"};
    }

    it->second.state = to;

    bool invalidated = (to == ClusterNodeState::Down ||
                        to == ClusterNodeState::Quarantined ||
                        to == ClusterNodeState::Removed);

    if (invalidated) {
        invalidation_queue_.push_back(node_id);
    }

    return {true, invalidated, ""};
}

ClusterNodeState
ClusterFailureModel::get_state(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return ClusterNodeState::Removed;
    }
    return it->second.state;
}

size_t ClusterFailureModel::node_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.size();
}

IdentityCheckStatus ClusterFailureModel::check_identity_conflict(
    const ClusterNodeIdentity& received) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = nodes_.find(received.node_id);
    if (it == nodes_.end()) {
        return {}; // No existing node, no conflict
    }

    const auto& existing = it->second.identity;

    if (is_identity_conflict(received, existing)) {
        return {true, IdentityConflictResolution::QuarantineBoth};
    }

    if (fences(received, existing)) {
        return {false, IdentityConflictResolution::FenceOld};
    }

    return {};
}

PartitionPolicy ClusterFailureModel::get_partition_policy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partition_policy_;
}

void ClusterFailureModel::set_partition_policy(PartitionPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    partition_policy_ = policy;
}

bool ClusterFailureModel::quorum_present() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.empty()) return false;

    size_t alive_count = 0;
    for (const auto& [id, record] : nodes_) {
        if (record.state == ClusterNodeState::Alive) {
            ++alive_count;
        }
    }
    // Quorum is more than half of registered nodes
    return alive_count > (nodes_.size() / 2);
}

std::vector<std::string> ClusterFailureModel::alive_nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& [id, record] : nodes_) {
        if (record.state == ClusterNodeState::Alive) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<std::string>
ClusterFailureModel::drain_invalidation_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> drained;
    drained.swap(invalidation_queue_);
    return drained;
}

} // namespace hpactor::cluster
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="ClusterFailureModelTest*"
```

Expected: All 11 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cluster/cluster_failure_model.hpp src/cluster/cluster_failure_model.cpp tests/unit/cluster/test_cluster_route_invalidation.cpp
git commit -m "feat(cluster): add ClusterFailureModel policy engine

Owns authoritative node state map. Manages state transitions with
legal-transition validation. Detects identity conflicts (duplicate
process, higher incarnation fencing). Tracks invalidation queue for
route purging. Thread-safe via internal mutex.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 10: RouteInvalidation — Cache Purge + Op-Failure Helpers (RED)

**Files:**
- Create: `include/hpactor/cluster/route_invalidation.hpp`
- Create: `src/cluster/route_invalidation.cpp`
- Existing test: `tests/unit/cluster/test_cluster_route_invalidation.cpp` (already created in Task 8)

- [ ] **Step 1: Extend the existing test file — add to `tests/unit/cluster/test_cluster_route_invalidation.cpp`**

Append after the Task 8 tests (after the `}` closing the namespace):

```cpp
TEST(RouteInvalidationTest, InvalidateTriggersOnDown) {
    EXPECT_TRUE(should_invalidate_routes(ClusterNodeState::Down));
}

TEST(RouteInvalidationTest, InvalidateTriggersOnQuarantined) {
    EXPECT_TRUE(should_invalidate_routes(ClusterNodeState::Quarantined));
}

TEST(RouteInvalidationTest, InvalidateTriggersOnRemoved) {
    EXPECT_TRUE(should_invalidate_routes(ClusterNodeState::Removed));
}

TEST(RouteInvalidationTest, NoInvalidateOnAlive) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Alive));
}

TEST(RouteInvalidationTest, NoInvalidateOnSuspect) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Suspect));
}

TEST(RouteInvalidationTest, NoInvalidateOnJoining) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Joining));
}

TEST(RouteInvalidationTest, NoInvalidateOnLeaving) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Leaving));
}

TEST(RouteInvalidationTest, NoInvalidateOnUnreachable) {
    // Unreachable may recover — don't invalidate yet
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Unreachable));
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_cluster 2>&1 | tail -10
```

Expected: Compilation fails — `should_invalidate_routes()` not declared.

---

## Task 11: RouteInvalidation — Implementation (GREEN)

**Files:**
- Create: `include/hpactor/cluster/route_invalidation.hpp`
- Create: `src/cluster/route_invalidation.cpp`

- [ ] **Step 1: Write the header — `include/hpactor/cluster/route_invalidation.hpp`**

```cpp
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

#include <hpactor/cluster/cluster_node_state.hpp>

#include <string>
#include <vector>

namespace hpactor::cluster {

/// \brief Whether transitioning to this state should trigger route
///        invalidation (cache purge, pending-op failure, connection close).
///
/// \param[in] state The target state.
/// \return true if routes should be invalidated for this state.
constexpr bool should_invalidate_routes(ClusterNodeState state) noexcept {
    return state == ClusterNodeState::Down ||
           state == ClusterNodeState::Quarantined ||
           state == ClusterNodeState::Removed;
}

/// \brief Orchestrates route invalidation across multiple subsystems.
///
/// When a node becomes unreachable for routing (Down, Quarantined, Removed),
/// this class coordinates the cleanup: purging ActorLocationCache entries,
/// failing pending RPC/spawn operations, and closing connections.
///
/// This is a coordinator — actual invalidation is carried out by the
/// respective subsystems (ActorLocationCache, ConnectionPool, RpcChannel).
class RouteInvalidation {
public:
    RouteInvalidation() = default;

    /// \brief Process the invalidation queue from the failure model.
    ///
    /// Called periodically or after state changes. Drains the invalidation
    /// queue and invokes registered invalidation callbacks.
    ///
    /// \param[in] node_ids Nodes that have been invalidated since last call.
    void process(const std::vector<std::string>& node_ids);

    /// \brief Register a callback invoked for each invalidated node.
    ///
    /// Subsystems (ActorLocationCache, ConnectionPool, etc.) register
    /// callbacks at initialization time.
    ///
    /// \param[in] callback Function(NodeId) called for each invalidated node.
    void register_callback(std::function<void(const std::string&)> callback);

private:
    std::vector<std::function<void(const std::string&)>> callbacks_;
};

} // namespace hpactor::cluster
```

- [ ] **Step 2: Write the implementation — `src/cluster/route_invalidation.cpp`**

```cpp
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

#include <hpactor/cluster/route_invalidation.hpp>

namespace hpactor::cluster {

void RouteInvalidation::process(const std::vector<std::string>& node_ids) {
    for (const auto& node_id : node_ids) {
        for (const auto& cb : callbacks_) {
            cb(node_id);
        }
    }
}

void RouteInvalidation::register_callback(
    std::function<void(const std::string&)> callback) {
    callbacks_.push_back(std::move(callback));
}

} // namespace hpactor::cluster
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="RouteInvalidationTest*"
```

Expected: All 8 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cluster/route_invalidation.hpp src/cluster/route_invalidation.cpp tests/unit/cluster/test_cluster_route_invalidation.cpp
git commit -m "feat(cluster): add RouteInvalidation coordinator

should_invalidate_routes() predicate gates invalidation to Down,
Quarantined, and Removed states. RouteInvalidation class orchestrates
cache purge + op-failure + connection-close via registered callbacks.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 12: FailureReason Extensions — New Codes (RED → GREEN)

**File:**
- Modify: `include/hpactor/msg/failure_reason.hpp`

- [ ] **Step 1: Add new FailureReason codes and update retryable()**

Edit `include/hpactor/msg/failure_reason.hpp`:

In the Route range (after `RemoteUnavailable = 2`), add `NodeQuarantined = 3` and `NodeReplaced = 4`:

```cpp
// ── Route / addressing (0-9) ────────────────────────────────
NoRoute = 0,
NodeUnavailable = 1,
RemoteUnavailable = 2,
NodeQuarantined = 3, ///< Remote node is quarantined (identity/epoch conflict).
NodeReplaced = 4,    ///< Remote node was replaced by a higher-incarnation peer.
```

In the Transport range (after `FrameRejected = 52`), add `FencingTokenStale = 53`:

```cpp
// ── Transport / serialization (50-59) ───────────────────────
SerializationError = 50,
TransportError = 51,
FrameRejected = 52,
FencingTokenStale = 53, ///< Singleton or shard fencing token is stale; write
                         ///< rejected.
```

Update `retryable()` function to include `NodeQuarantined` (NOT retryable — requires operator intervention) and `NodeReplaced` (retryable — new peer should work):

```cpp
constexpr bool retryable(FailureReason reason) noexcept {
    switch (reason) {
        // ... existing cases ...
        case FailureReason::NodeReplaced:
        case FailureReason::FencingTokenStale:
            return true;
        case FailureReason::NodeQuarantined:
            return false;
        // ... rest unchanged ...
    }
}
```

Update `to_string()` implementation in `src/msg/failure_reason.cpp`:

```cpp
case FailureReason::NodeQuarantined:  return "node_quarantined";
case FailureReason::NodeReplaced:     return "node_replaced";
case FailureReason::FencingTokenStale: return "fencing_token_stale";
```

- [ ] **Step 2: Check `to_string()` implementation location**

```bash
grep -rn "case FailureReason::NodeUnavailable" src/ include/hpactor/
```

Expected: Finds the `to_string()` implementation. Add the new cases adjacent.

- [ ] **Step 3: Build and verify compilation**

```bash
ninja -C build hpactor_cluster
```

Expected: Build succeeds.

- [ ] **Step 4: Run all cluster tests to verify nothing broken**

```bash
ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster
```

Expected: All ~36 tests pass (21 state + 8 identity + 7 partition + 11 model + 8 invalidation = 55 tests — corrected).

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/failure_reason.hpp src/msg/failure_reason.cpp
git commit -m "feat(cluster): add NodeQuarantined, NodeReplaced, FencingTokenStale failure reasons

New FailureReason codes for cluster control-plane operations:
NodeQuarantined (route 3), NodeReplaced (route 4),
FencingTokenStale (transport 53). Updated retryable() and to_string().

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 13: ActorLocationCache — Add purge_node() (RED → GREEN)

**Files:**
- Modify: `include/hpactor/net/actor_location_cache.hpp`
- Modify: `src/net/actor_location_cache.cpp`

- [ ] **Step 1: Add method declaration to header**

In `include/hpactor/net/actor_location_cache.hpp`, after the `evict_node` declaration, add:

```cpp
/// \brief Purge all cache entries pointing to a specific node.
///
/// Called by ClusterFailureModel route invalidation when a node enters
/// Down, Quarantined, or Removed state.
///
/// \param[in] node_id The logical node ID to purge.
void purge_node(const std::string& node_id);
```

- [ ] **Step 2: Add implementation in `src/net/actor_location_cache.cpp`**

```cpp
void ActorLocationCache::purge_node(const std::string& node_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Iterate and remove entries where the endpoint's node matches node_id.
    // This uses evict_node() semantics — matching by node identity rather
    // than exact endpoint.
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (it->second.endpoint.node_id() == node_id) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}
```

Note: Verify the `EndPoint` class has a `node_id()` accessor. If not, use the appropriate method to extract the node identifier from the endpoint.

- [ ] **Step 3: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/actor_location_cache.hpp src/net/actor_location_cache.cpp
git commit -m "feat(net): add ActorLocationCache::purge_node() for route invalidation

Purges all cached ActorId→Endpoint entries for a given node ID.
Called by ClusterFailureModel when a node transitions to Down,
Quarantined, or Removed.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 14: Final Integration Build & Full Test Run

- [ ] **Step 1: Full build**

```bash
cd /Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/akka-gap-closure-sprint2
ninja -C build
```

Expected: All targets build. No regressions in existing libraries.

- [ ] **Step 2: Run cluster unit tests**

```bash
./build/tests/unit/cluster/test_unit_cluster
```

Expected: All ~55 tests pass.

- [ ] **Step 3: Run existing regression tests (spot-check)**

```bash
ctest --test-dir build -R "test_unit_core|test_unit_actor|test_unit_mailbox" --output-on-failure --parallel 8
```

Expected: No regressions.

- [ ] **Step 4: Commit final integration**

```bash
git add -A
git commit -m "feat(cluster): complete CLU-001 Cluster Failure Model & Fencing

Implements the foundational cluster control-plane subsystem:
- ClusterNodeState: 8-state model (Joining → Removed) with constexpr
  transition table, is_alive(), is_terminal(), can_transition()
- ClusterNodeIdentity: fencing via incarnation, process_start_id, epoch,
  cluster_id; same_cluster() gating, is_identity_conflict() detection
- PartitionPolicy: FailOpen, FailClosed, StaticMajority with
  allow_user_delivery(), allow_ownership_change() decision helpers
- ClusterFailureModel: policy engine owning node state map;
  register_node(), transition(), check_identity_conflict(),
  quorum_present(), alive_nodes()
- RouteInvalidation: callback-based coordinator; should_invalidate_routes()
  gates to Down/Quarantined/Removed
- ActorLocationCache::purge_node() for cache invalidation
- FailureReason extensions: NodeQuarantined (3), NodeReplaced (4),
  FencingTokenStale (53)
- 55 unit tests covering state transitions, identity fencing, partition
  decisions, model behavior, and route invalidation predicates

PR #1 of Sprint 2 Akka gap closure.
Design: docs/superpowers/specs/2026-06-21-akka-gap-closure-sprint2-design.md

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Post-PR #1: Next Steps

After PR #1 is reviewed and merged, the following PRs will be planned and implemented:

| PR | Subsystem | Depends On | Key Component |
|----|-----------|------------|---------------|
| 2 | CLU-002 Cluster Sharding | CLU-001 | ShardResolver, ShardTable, ShardCoordinator, PlacementStrategy, ShardHandoff |
| 3 | CLU-003 Cluster Singleton | CLU-001, CLU-002 | SingletonManager, SingletonElection, OldestNodeElection, FencingToken |
| 4 | MSG-003 Reliable Messaging | DeliveryMode, DedupCache | OutboundTracker, ACK/NACK frames, ReliableRetryPolicy |

Each subsequent PR will receive its own detailed implementation plan following the same TDDFlow structure.

---

## Summary: CLU-001 Task Count

| Task | Name | Test State |
|------|------|------------|
| 1 | Build Infrastructure — CMake scaffold | — |
| 2 | ClusterNodeState — RED | 21 tests fail (compile) |
| 3 | ClusterNodeState — GREEN | 21 tests pass |
| 4 | ClusterNodeIdentity — RED | 8 tests fail (compile) |
| 5 | ClusterNodeIdentity — GREEN | 8 tests pass |
| 6 | PartitionPolicy — RED | 7 tests fail (compile) |
| 7 | PartitionPolicy — GREEN | 7 tests pass |
| 8 | ClusterFailureModel — RED | 11 tests fail (compile) |
| 9 | ClusterFailureModel — GREEN | 11 tests pass |
| 10 | RouteInvalidation — RED | 8 tests fail (compile) |
| 11 | RouteInvalidation — GREEN | 8 tests pass |
| 12 | FailureReason extensions | — |
| 13 | ActorLocationCache::purge_node() | — |
| 14 | Final integration build & test | All ~55 tests pass |

**Total: 14 tasks, ~55 unit tests, ~10 files created, ~4 files modified**
