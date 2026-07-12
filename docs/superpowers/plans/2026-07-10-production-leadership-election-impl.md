# Production Distributed Leadership Election — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add production consensus-backed singleton leadership (etcd-first, Consul-compatible, Raft-future) through an `ILeadershipBackend` contract and `LeadershipBackendAdapter` that drops into the existing `ISingletonElection` seam without changing `SingletonManagerCore`'s public API.

**Architecture:** Adapter pattern — `LeadershipBackendAdapter` implements `ISingletonElection`, translating `elect()` into backend `try_acquire()` calls. `SingletonManagerCore` queries `election_->get_fencing_token()` to source the backend token instead of locally incrementing. A deterministic `FakeLeadershipBackend` enables unit testing without network dependencies. `ClusterRuntimeImpl` selects the election strategy from TOML config.

**Tech Stack:** C++20, protobuf TypedMessage, Google Test, BehaviorTestKit, no exceptions/RTTI, deterministic tests (no wall-clock timing in unit tests).

## Global Constraints

- Worktree isolation: all writes in `.claude/worktrees/<name>/`, never main checkout
- TDDFlow: RED → GREEN → REFACTOR before any production code
- No exceptions, no RTTI, no `dynamic_cast` or `typeid`
- Core/Actor separation: thread-safe core class + thin actor wrapper
- Deterministic tests: no real threads, no wall-clock timing, no sleep-based waits
- Subsystem-owned TOML config: self-registering parser in `src/config/parsers/`
- Opaque `TomlTableView` — never expose `toml++` in public headers
- All existing 98 cluster tests must pass unchanged

---
## File Map

```
include/hpactor/cluster/singleton/
├── singleton_election.hpp          [MODIFY] Add get_fencing_token() virtual
├── singleton_identity.hpp          [MODIFY] Add is_backend_token flag (or keep as-is)
├── singleton_manager.hpp           [MODIFY] Use backend token when available
├── singleton_manager_actor.hpp     [NO CHANGE]
├── oldest_node_election.hpp        [NO CHANGE]
├── majority_based_election.hpp     [NO CHANGE]
├── fixed_priority_election.hpp     [NO CHANGE]
├── leadership_lease.hpp            [CREATE]  LeadershipLease value type
├── leadership_status.hpp           [CREATE]  LeadershipStatusCode + LeadershipResult
├── leadership_backend.hpp          [CREATE]  ILeadershipBackend interface
├── leadership_backend_adapter.hpp  [CREATE]  Adapter: ISingletonElection over ILeadershipBackend
└── fake_leadership_backend.hpp     [CREATE]  Deterministic fake for tests

src/cluster/singleton/
├── singleton_manager.cpp           [MODIFY] fencing token from election strategy
├── leadership_lease.cpp            [CREATE]  Comparison operators, serialization
├── leadership_backend_adapter.cpp  [CREATE]  Adapter implementation
└── fake_leadership_backend.cpp     [CREATE]  Fake backend implementation

src/cluster/
├── CMakeLists.txt                  [MODIFY] Add new source files
├── cluster_runtime_impl.cpp        [MODIFY] Wire leadership config → election choice

src/config/parsers/
└── cluster_leadership_parser.cpp   [CREATE]  TOML parser for [system.cluster.leadership]

tests/unit/cluster/singleton/
├── test_leadership_lease.cpp       [CREATE]  LeadershipLease unit tests
├── test_leadership_status.cpp      [CREATE]  LeadershipStatus unit tests
├── test_fake_leadership_backend.cpp[CREATE]  Fake backend unit tests
├── test_leadership_backend_adapter.cpp [CREATE] Adapter unit tests
├── test_singleton_manager_leader.cpp   [CREATE] SingletonManagerCore + adapter tests

tests/unit/cluster/
├── CMakeLists.txt                  [MODIFY] Add new test files
```

---
## M1: Core Types + Fake Backend + Adapter Integration

**Goal:** End-to-end production leadership contract working with deterministic fake backend. All existing tests pass. No network dependency.

---

### Task 1: LeadershipLease value type

**Files:**
- Create: `include/hpactor/cluster/singleton/leadership_lease.hpp`
- Create: `src/cluster/singleton/leadership_lease.cpp`
- Create: `tests/unit/cluster/singleton/test_leadership_lease.cpp`

**Interfaces:**
- Produces: `LeadershipLease` struct with `operator<`, `operator==`, `operator!=`, `operator<=`, `is_expired(now)`, `fences(other)` (true if this token is strictly greater)
- Fields: `cluster_id` (string), `singleton_name` (string), `owner_node_id` (string), `owner_incarnation` (uint64_t), `owner_process_start_id` (uint64_t), `membership_epoch` (uint64_t), `fencing_token` (uint64_t), `backend_term` (uint64_t), `backend_revision` (uint64_t), `lease_deadline` (Clock::time_point)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/cluster/singleton/test_leadership_lease.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <chrono>

namespace hpactor::cluster::singleton {

using Clock = std::chrono::steady_clock;

LeadershipLease make_lease(const std::string& singleton_name,
                           const std::string& owner,
                           uint64_t token,
                           Clock::time_point deadline) {
    LeadershipLease l;
    l.cluster_id = "test-cluster";
    l.singleton_name = singleton_name;
    l.owner_node_id = owner;
    l.owner_incarnation = 1;
    l.owner_process_start_id = 100;
    l.membership_epoch = 1;
    l.fencing_token = token;
    l.backend_term = 0;
    l.backend_revision = token;
    l.lease_deadline = deadline;
    return l;
}

TEST(LeadershipLeaseTest, DefaultConstructionHasZeroToken) {
    LeadershipLease lease;
    EXPECT_TRUE(lease.cluster_id.empty());
    EXPECT_EQ(lease.fencing_token, 0u);
}

TEST(LeadershipLeaseTest, HigherTokenIsGreater) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 1, deadline);
    auto b = make_lease("s1", "node-b", 2, deadline);
    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_NE(a, b);
}

TEST(LeadershipLeaseTest, SameTokenDifferentSingletonNotComparable) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 5, deadline);
    auto b = make_lease("s2", "node-b", 5, deadline);
    // Same token, different singleton — comparison is by singleton_name then token
    EXPECT_NE(a, b);
}

TEST(LeadershipLeaseTest, FencesReturnsTrueWhenTokenGreater) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto old_lease = make_lease("s1", "node-a", 3, deadline);
    auto new_lease = make_lease("s1", "node-b", 7, deadline);
    EXPECT_TRUE(new_lease.fences(old_lease));
    EXPECT_FALSE(old_lease.fences(new_lease));
}

TEST(LeadershipLeaseTest, FencesReturnsFalseWhenSameToken) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 5, deadline);
    auto b = make_lease("s1", "node-b", 5, deadline);
    EXPECT_FALSE(b.fences(a));
}

TEST(LeadershipLeaseTest, IsExpiredReturnsTrueAfterDeadline) {
    auto past = Clock::now() - std::chrono::seconds(1);
    auto l = make_lease("s1", "node-a", 1, past);
    EXPECT_TRUE(l.is_expired(Clock::now()));
}

TEST(LeadershipLeaseTest, IsExpiredReturnsFalseBeforeDeadline) {
    auto future = Clock::now() + std::chrono::seconds(60);
    auto l = make_lease("s1", "node-a", 1, future);
    EXPECT_FALSE(l.is_expired(Clock::now()));
}

TEST(LeadershipLeaseTest, OperatorLessEqual) {
    auto now = Clock::now();
    auto deadline = now + std::chrono::seconds(10);
    auto a = make_lease("s1", "node-a", 1, deadline);
    auto b = make_lease("s1", "node-a", 1, deadline);
    EXPECT_LE(a, b);
    auto c = make_lease("s1", "node-b", 2, deadline);
    EXPECT_LE(a, c);
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*LeadershipLease*"`
Expected: FAIL — `leadership_lease.hpp` not found

- [ ] **Step 3: Write the header**

```cpp
// include/hpactor/cluster/singleton/leadership_lease.hpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor::cluster::singleton {

using Clock = std::chrono::steady_clock;

/// \brief A backend-issued leadership lease for a cluster singleton.
///
/// Grants exactly-one ownership semantics. The fencing token is monotonically
/// ordered by the backend for a given (cluster_id, singleton_name) pair.
/// Consumers compare fencing_token to reject stale commands.
struct LeadershipLease {
    std::string cluster_id;
    std::string singleton_name;
    std::string owner_node_id;
    uint64_t owner_incarnation = 0;
    uint64_t owner_process_start_id = 0;
    uint64_t membership_epoch = 0;
    uint64_t fencing_token = 0;
    uint64_t backend_term = 0;
    uint64_t backend_revision = 0;
    Clock::time_point lease_deadline{};

    /// \brief True if this lease's fencing token strictly dominates \p other
    /// for the same singleton.
    [[nodiscard]] bool fences(const LeadershipLease& other) const noexcept;

    /// \brief True if the lease deadline has passed.
    [[nodiscard]] bool is_expired(Clock::time_point now) const noexcept;

    friend bool operator<(const LeadershipLease& a, const LeadershipLease& b) noexcept;
    friend bool operator==(const LeadershipLease& a, const LeadershipLease& b) noexcept;
    friend bool operator!=(const LeadershipLease& a, const LeadershipLease& b) noexcept;
    friend bool operator<=(const LeadershipLease& a, const LeadershipLease& b) noexcept;
};

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/cluster/singleton/leadership_lease.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/singleton/leadership_lease.hpp>

namespace hpactor::cluster::singleton {

bool LeadershipLease::fences(const LeadershipLease& other) const noexcept {
    if (singleton_name != other.singleton_name) return false;
    if (cluster_id != other.cluster_id) return false;
    return fencing_token > other.fencing_token;
}

bool LeadershipLease::is_expired(Clock::time_point now) const noexcept {
    return now >= lease_deadline;
}

bool operator<(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    if (a.cluster_id != b.cluster_id) return a.cluster_id < b.cluster_id;
    if (a.singleton_name != b.singleton_name) return a.singleton_name < b.singleton_name;
    return a.fencing_token < b.fencing_token;
}

bool operator==(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    return a.cluster_id == b.cluster_id
        && a.singleton_name == b.singleton_name
        && a.fencing_token == b.fencing_token
        && a.owner_node_id == b.owner_node_id;
}

bool operator!=(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    return !(a == b);
}

bool operator<=(const LeadershipLease& a, const LeadershipLease& b) noexcept {
    return a < b || a == b;
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 5: Add source to CMakeLists.txt**

```cmake
# In src/cluster/CMakeLists.txt, add after singleton_manager_actor.cpp:
    singleton/leadership_lease.cpp
```

- [ ] **Step 6: Run test to verify it passes**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*LeadershipLease*"`
Expected: 8 tests PASS

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cluster/singleton/leadership_lease.hpp \
        src/cluster/singleton/leadership_lease.cpp \
        src/cluster/CMakeLists.txt \
        tests/unit/cluster/singleton/test_leadership_lease.cpp \
        tests/unit/cluster/CMakeLists.txt
git commit -m "feat(cluster): add LeadershipLease value type with fencing and comparison"
```

---

### Task 2: LeadershipStatus + LeadershipResult types

**Files:**
- Create: `include/hpactor/cluster/singleton/leadership_status.hpp`
- Create: `tests/unit/cluster/singleton/test_leadership_status.cpp`

**Interfaces:**
- Produces: `LeadershipStatusCode` enum (Granted, AlreadyOwned, Renewed, Released, Lost, NotOwner, BackendUnavailable, StaleMembershipEpoch, IdentityRejected, PermissionDenied, TimedOut)
- Produces: `LeadershipResult` struct with `status` (LeadershipStatusCode), `lease` (std::optional<LeadershipLease>), `current_owner` (std::optional<std::string>)
- Produces: `to_string(LeadershipStatusCode)` free function

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/cluster/singleton/test_leadership_status.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/leadership_status.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>

namespace hpactor::cluster::singleton {

TEST(LeadershipStatusTest, GrantedCarriesLease) {
    LeadershipLease lease;
    lease.singleton_name = "s1";
    lease.fencing_token = 42;
    LeadershipResult r = LeadershipResult::granted(lease);
    EXPECT_EQ(r.status, LeadershipStatusCode::Granted);
    ASSERT_TRUE(r.lease.has_value());
    EXPECT_EQ(r.lease->fencing_token, 42u);
}

TEST(LeadershipStatusTest, BackendUnavailableCarriesNoLease) {
    LeadershipResult r = LeadershipResult::unavailable();
    EXPECT_EQ(r.status, LeadershipStatusCode::BackendUnavailable);
    EXPECT_FALSE(r.lease.has_value());
}

TEST(LeadershipStatusTest, AlreadyOwnedCarriesCurrentOwner) {
    LeadershipLease lease;
    lease.owner_node_id = "node-b";
    LeadershipResult r = LeadershipResult::already_owned("node-b", lease);
    EXPECT_EQ(r.status, LeadershipStatusCode::AlreadyOwned);
    ASSERT_TRUE(r.current_owner.has_value());
    EXPECT_EQ(*r.current_owner, "node-b");
}

TEST(LeadershipStatusTest, LostCarriesReason) {
    LeadershipResult r = LeadershipResult::lost();
    EXPECT_EQ(r.status, LeadershipStatusCode::Lost);
    EXPECT_FALSE(r.lease.has_value());
}

TEST(LeadershipStatusTest, ReleasedIsSuccess) {
    LeadershipResult r = LeadershipResult::released();
    EXPECT_EQ(r.status, LeadershipStatusCode::Released);
}

TEST(LeadershipStatusTest, RenewedCarriesLease) {
    LeadershipLease lease;
    lease.fencing_token = 99;
    LeadershipResult r = LeadershipResult::renewed(lease);
    EXPECT_EQ(r.status, LeadershipStatusCode::Renewed);
    ASSERT_TRUE(r.lease.has_value());
    EXPECT_EQ(r.lease->fencing_token, 99u);
}

TEST(LeadershipStatusTest, AllStatusCodesHaveDistinctValues) {
    EXPECT_NE(static_cast<uint8_t>(LeadershipStatusCode::Granted),
              static_cast<uint8_t>(LeadershipStatusCode::Lost));
    EXPECT_NE(static_cast<uint8_t>(LeadershipStatusCode::BackendUnavailable),
              static_cast<uint8_t>(LeadershipStatusCode::TimedOut));
    EXPECT_NE(static_cast<uint8_t>(LeadershipStatusCode::IdentityRejected),
              static_cast<uint8_t>(LeadershipStatusCode::PermissionDenied));
}

TEST(LeadershipStatusTest, ToStringReturnsExpected) {
    EXPECT_STREQ(to_string(LeadershipStatusCode::Granted), "granted");
    EXPECT_STREQ(to_string(LeadershipStatusCode::Lost), "lost");
    EXPECT_STREQ(to_string(LeadershipStatusCode::BackendUnavailable), "backend_unavailable");
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*LeadershipStatus*"`
Expected: FAIL — `leadership_status.hpp` not found

- [ ] **Step 3: Write the header**

```cpp
// include/hpactor/cluster/singleton/leadership_status.hpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace hpactor::cluster::singleton {

/// \brief Outcome codes for ILeadershipBackend operations.
enum class LeadershipStatusCode : uint8_t {
    Granted,
    AlreadyOwned,
    Renewed,
    Released,
    Lost,
    NotOwner,
    BackendUnavailable,
    StaleMembershipEpoch,
    IdentityRejected,
    PermissionDenied,
    TimedOut,
};

/// \brief Human-readable snake_case string for the status code.
const char* to_string(LeadershipStatusCode code) noexcept;

/// \brief Result of a leadership backend operation.
///
/// Carries the lease on success (Granted, Renewed) and the current
/// owner on AlreadyOwned. Lost, BackendUnavailable, and other failures
/// carry no lease.
struct LeadershipResult {
    LeadershipStatusCode status = LeadershipStatusCode::NotOwner;
    std::optional<LeadershipLease> lease;
    std::optional<std::string> current_owner;

    static LeadershipResult granted(LeadershipLease l) {
        LeadershipResult r;
        r.status = LeadershipStatusCode::Granted;
        r.lease = std::move(l);
        return r;
    }
    static LeadershipResult renewed(LeadershipLease l) {
        LeadershipResult r;
        r.status = LeadershipStatusCode::Renewed;
        r.lease = std::move(l);
        return r;
    }
    static LeadershipResult already_owned(std::string owner, LeadershipLease l) {
        LeadershipResult r;
        r.status = LeadershipStatusCode::AlreadyOwned;
        r.current_owner = std::move(owner);
        r.lease = std::move(l);
        return r;
    }
    static LeadershipResult released() {
        return {LeadershipStatusCode::Released, std::nullopt, std::nullopt};
    }
    static LeadershipResult lost() {
        return {LeadershipStatusCode::Lost, std::nullopt, std::nullopt};
    }
    static LeadershipResult unavailable() {
        return {LeadershipStatusCode::BackendUnavailable, std::nullopt, std::nullopt};
    }
    static LeadershipResult stale_epoch() {
        return {LeadershipStatusCode::StaleMembershipEpoch, std::nullopt, std::nullopt};
    }
    static LeadershipResult identity_rejected() {
        return {LeadershipStatusCode::IdentityRejected, std::nullopt, std::nullopt};
    }
    static LeadershipResult timed_out() {
        return {LeadershipStatusCode::TimedOut, std::nullopt, std::nullopt};
    }

    [[nodiscard]] bool is_success() const noexcept {
        return status == LeadershipStatusCode::Granted
            || status == LeadershipStatusCode::Renewed
            || status == LeadershipStatusCode::Released;
    }
};

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/cluster/singleton/leadership_status.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/singleton/leadership_status.hpp>

namespace hpactor::cluster::singleton {

const char* to_string(LeadershipStatusCode code) noexcept {
    switch (code) {
        case LeadershipStatusCode::Granted:              return "granted";
        case LeadershipStatusCode::AlreadyOwned:          return "already_owned";
        case LeadershipStatusCode::Renewed:               return "renewed";
        case LeadershipStatusCode::Released:              return "released";
        case LeadershipStatusCode::Lost:                  return "lost";
        case LeadershipStatusCode::NotOwner:              return "not_owner";
        case LeadershipStatusCode::BackendUnavailable:    return "backend_unavailable";
        case LeadershipStatusCode::StaleMembershipEpoch:  return "stale_membership_epoch";
        case LeadershipStatusCode::IdentityRejected:      return "identity_rejected";
        case LeadershipStatusCode::PermissionDenied:      return "permission_denied";
        case LeadershipStatusCode::TimedOut:              return "timed_out";
    }
    return "unknown";
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 5: Add source to CMakeLists.txt**

```cmake
# In src/cluster/CMakeLists.txt, add after singleton/leadership_lease.cpp:
    singleton/leadership_status.cpp
```

- [ ] **Step 6: Run test to verify it passes**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*LeadershipStatus*"`
Expected: 8 tests PASS

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cluster/singleton/leadership_status.hpp \
        src/cluster/singleton/leadership_status.cpp \
        tests/unit/cluster/singleton/test_leadership_status.cpp \
        src/cluster/CMakeLists.txt tests/unit/cluster/CMakeLists.txt
git commit -m "feat(cluster): add LeadershipStatus and LeadershipResult types"
```

---

### Task 3: ILeadershipBackend interface

**Files:**
- Create: `include/hpactor/cluster/singleton/leadership_backend.hpp`

**Interfaces:**
- Produces: `ILeadershipBackend` abstract class with:
  - `virtual ~ILeadershipBackend() = default`
  - `virtual LeadershipResult try_acquire(const LeadershipAttempt& attempt) = 0`
  - `virtual LeadershipResult renew(const LeadershipLease& lease) = 0`
  - `virtual LeadershipResult release(const LeadershipLease& lease) = 0`
  - `virtual LeadershipResult current_owner(std::string_view singleton_name) = 0`
- Consumes: `LeadershipLease` (Task 1), `LeadershipResult` (Task 2)
- Produces: `LeadershipAttempt` struct with `singleton_name`, `self_node_id`, `observed_membership_epoch`, `lease_ttl`

- [ ] **Step 1: Write the header (no separate test — compile-only verification; interface has no behavior)**

```cpp
// include/hpactor/cluster/singleton/leadership_backend.hpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_lease.hpp>
#include <hpactor/cluster/singleton/leadership_status.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace hpactor::cluster::singleton {

/// \brief Parameters for a leadership acquisition attempt.
struct LeadershipAttempt {
    std::string singleton_name;
    std::string self_node_id;
    uint64_t observed_membership_epoch = 0;
    Clock::duration lease_ttl{};
};

/// \brief Abstract backend for distributed singleton leadership.
///
/// Implementations provide linearizable ownership through etcd, Consul,
/// or internal Raft. All calls return explicit LeadershipResult values.
/// No exception-based control flow. Backend implementations run on
/// dedicated executors — never on scheduler or event-loop hot paths.
class ILeadershipBackend {
  public:
    virtual ~ILeadershipBackend() = default;

    /// \brief Attempt to acquire leadership for a singleton.
    virtual LeadershipResult try_acquire(const LeadershipAttempt& attempt) = 0;

    /// \brief Renew an existing lease before it expires.
    virtual LeadershipResult renew(const LeadershipLease& lease) = 0;

    /// \brief Voluntarily release a lease.
    virtual LeadershipResult release(const LeadershipLease& lease) = 0;

    /// \brief Query the current owner of a singleton.
    virtual LeadershipResult current_owner(std::string_view singleton_name) = 0;
};

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Build to verify compilation**

Run: `ninja -C build`
Expected: Compiles clean (no separate test binary needed — interface is pure virtual)

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/cluster/singleton/leadership_backend.hpp
git commit -m "feat(cluster): add ILeadershipBackend interface for distributed leadership"
```

---

### Task 4: Extend ISingletonElection with get_fencing_token()

**Files:**
- Modify: `include/hpactor/cluster/singleton/singleton_election.hpp`

**Interfaces:**
- Produces: `virtual uint64_t get_fencing_token(std::string_view singleton_name) const` on `ISingletonElection` — default returns 0
- Existing elect() and on_peer_down() unchanged

- [ ] **Step 1: Verify existing tests pass first**

Run: `ctest -R "Singleton|Election" --output-on-failure`
Expected: All existing singleton and election tests PASS

- [ ] **Step 2: Add the virtual method**

In `include/hpactor/cluster/singleton/singleton_election.hpp`, add after `on_peer_down()`:

```cpp
/// \brief Get the fencing token for a singleton owned by this node.
///
/// Returns the backend-issued token when backed by an ILeadershipBackend.
/// Default returns 0 — callers fall back to local increment.
///
/// \param[in] singleton_name The singleton to query.
/// \return The current fencing token, or 0 if not backend-managed.
virtual uint64_t get_fencing_token(std::string_view /*singleton_name*/) const {
    return 0;
}
```

- [ ] **Step 3: Run existing tests to verify no regression**

Run: `ctest -R "Singleton|Election" --output-on-failure`
Expected: All existing tests PASS (default returns 0, backward-compatible)

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/cluster/singleton/singleton_election.hpp
git commit -m "feat(cluster): add get_fencing_token() to ISingletonElection interface"
```

---

### Task 5: FakeLeadershipBackend

**Files:**
- Create: `include/hpactor/cluster/singleton/fake_leadership_backend.hpp`
- Create: `src/cluster/singleton/fake_leadership_backend.cpp`
- Create: `tests/unit/cluster/singleton/test_fake_leadership_backend.cpp`

**Interfaces:**
- Consumes: `ILeadershipBackend` (Task 3), `LeadershipLease` (Task 1), `LeadershipResult` (Task 2)
- Produces: `FakeLeadershipBackend` with test-control methods: `force_grant(singleton, owner, ttl)`, `force_revoke(singleton)`, `simulate_unavailable(bool)`, `get_grant_count(singleton)`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/cluster/singleton/test_fake_leadership_backend.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>

#include <chrono>

namespace hpactor::cluster::singleton {

using namespace std::chrono_literals;

class FakeLeadershipBackendTest : public ::testing::Test {
  protected:
    void SetUp() override {
        backend_ = std::make_unique<FakeLeadershipBackend>();
    }
    std::unique_ptr<FakeLeadershipBackend> backend_;
};

TEST_F(FakeLeadershipBackendTest, TryAcquireReturnsGrantedWhenNoOwner) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-a";
    attempt.lease_ttl = 10s;

    auto result = backend_->try_acquire(attempt);
    EXPECT_EQ(result.status, LeadershipStatusCode::Granted);
    ASSERT_TRUE(result.lease.has_value());
    EXPECT_EQ(result.lease->owner_node_id, "node-a");
    EXPECT_EQ(result.lease->singleton_name, "s1");
    EXPECT_GT(result.lease->fencing_token, 0u);
}

TEST_F(FakeLeadershipBackendTest, TryAcquireReturnsAlreadyOwnedForDifferentCaller) {
    backend_->force_grant("s1", "node-a", 10s);

    // node-b tries to acquire — already owned by node-a
    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-b";
    attempt.lease_ttl = 10s;

    auto result = backend_->try_acquire(attempt);
    EXPECT_EQ(result.status, LeadershipStatusCode::AlreadyOwned);
    ASSERT_TRUE(result.current_owner.has_value());
    EXPECT_EQ(*result.current_owner, "node-a");
}

TEST_F(FakeLeadershipBackendTest, RenewReturnsRenewedWhenTokenMatches) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-a";
    attempt.lease_ttl = 10s;
    auto acquired = backend_->try_acquire(attempt);
    ASSERT_TRUE(acquired.lease.has_value());

    auto result = backend_->renew(*acquired.lease);
    EXPECT_EQ(result.status, LeadershipStatusCode::Renewed);
    ASSERT_TRUE(result.lease.has_value());
    EXPECT_GT(result.lease->fencing_token, acquired.lease->fencing_token);
}

TEST_F(FakeLeadershipBackendTest, ReleaseFreesOwnership) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt;
    attempt.singleton_name = "s1";
    attempt.self_node_id = "node-a";
    attempt.lease_ttl = 10s;
    auto acquired = backend_->try_acquire(attempt);
    ASSERT_TRUE(acquired.lease.has_value());

    auto rel = backend_->release(*acquired.lease);
    EXPECT_EQ(rel.status, LeadershipStatusCode::Released);

    // Now someone else can acquire
    LeadershipAttempt attempt2{"s1", "node-b", 0, 10s};
    auto result2 = backend_->try_acquire(attempt2);
    EXPECT_EQ(result2.status, LeadershipStatusCode::Granted);
}

TEST_F(FakeLeadershipBackendTest, CurrentOwnerReturnsOwnerAfterGrant) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt{"s1", "node-a", 0, 10s};
    backend_->try_acquire(attempt);

    auto result = backend_->current_owner("s1");
    EXPECT_EQ(result.status, LeadershipStatusCode::Granted);
    ASSERT_TRUE(result.lease.has_value());
    EXPECT_EQ(result.lease->owner_node_id, "node-a");
}

TEST_F(FakeLeadershipBackendTest, CurrentOwnerReturnsNotOwnerWhenUnset) {
    auto result = backend_->current_owner("nonexistent");
    EXPECT_EQ(result.status, LeadershipStatusCode::NotOwner);
    EXPECT_FALSE(result.lease.has_value());
}

TEST_F(FakeLeadershipBackendTest, SimulateUnavailableReturnsBackendUnavailable) {
    backend_->simulate_unavailable(true);

    LeadershipAttempt attempt{"s1", "node-a", 0, 10s};
    auto result = backend_->try_acquire(attempt);
    EXPECT_EQ(result.status, LeadershipStatusCode::BackendUnavailable);
}

TEST_F(FakeLeadershipBackendTest, ForceRevokeClearsOwnership) {
    backend_->force_grant("s1", "node-a", 10s);

    LeadershipAttempt attempt{"s1", "node-a", 0, 10s};
    backend_->try_acquire(attempt);

    backend_->force_revoke("s1");

    auto result = backend_->current_owner("s1");
    EXPECT_EQ(result.status, LeadershipStatusCode::NotOwner);
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*FakeLeadershipBackend*"`
Expected: FAIL — `fake_leadership_backend.hpp` not found

- [ ] **Step 3: Write the header**

```cpp
// include/hpactor/cluster/singleton/fake_leadership_backend.hpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_backend.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

/// \brief Deterministic fake ILeadershipBackend for unit testing.
///
/// Test code pre-configures ownership via force_grant() and calls
/// simulate_unavailable() to exercise failure paths. Not thread-safe
/// by default — callers must serialize access in multi-threaded tests.
class FakeLeadershipBackend : public ILeadershipBackend {
  public:
    FakeLeadershipBackend() = default;

    // ── ILeadershipBackend ──────────────────────────────────────────

    LeadershipResult try_acquire(const LeadershipAttempt& attempt) override;
    LeadershipResult renew(const LeadershipLease& lease) override;
    LeadershipResult release(const LeadershipLease& lease) override;
    LeadershipResult current_owner(std::string_view singleton_name) override;

    // ── Test control surface ────────────────────────────────────────

    /// \brief Pre-configure ownership so the next try_acquire succeeds.
    void force_grant(const std::string& singleton_name,
                     const std::string& owner_node_id,
                     Clock::duration ttl);

    /// \brief Force-remove ownership for a singleton.
    void force_revoke(const std::string& singleton_name);

    /// \brief Toggle backend unavailability for all operations.
    void simulate_unavailable(bool unavailable);

    /// \brief Number of acquire attempts for a singleton.
    int get_grant_count(const std::string& singleton_name) const;

  private:
    struct StoredLease {
        LeadershipLease lease;
        bool owned = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoredLease> leases_;
    uint64_t next_token_ = 1;
    bool unavailable_ = false;
};

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/cluster/singleton/fake_leadership_backend.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>

namespace hpactor::cluster::singleton {

LeadershipResult
FakeLeadershipBackend::try_acquire(const LeadershipAttempt& attempt) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_) return LeadershipResult::unavailable();

    auto it = leases_.find(attempt.singleton_name);
    if (it != leases_.end() && it->second.owned) {
        // Already owned — check if it's us
        if (it->second.lease.owner_node_id == attempt.self_node_id) {
            // Idempotent — return existing lease
            LeadershipResult r;
            r.status = LeadershipStatusCode::Granted;
            r.lease = it->second.lease;
            return r;
        }
        return LeadershipResult::already_owned(
            it->second.lease.owner_node_id, it->second.lease);
    }

    // Grant new lease
    LeadershipLease lease;
    lease.cluster_id = "fake";
    lease.singleton_name = attempt.singleton_name;
    lease.owner_node_id = attempt.self_node_id;
    lease.membership_epoch = attempt.observed_membership_epoch;
    lease.fencing_token = next_token_++;
    lease.backend_revision = lease.fencing_token;
    lease.lease_deadline = Clock::now() + attempt.lease_ttl;

    StoredLease stored;
    stored.lease = lease;
    stored.owned = true;
    leases_[attempt.singleton_name] = stored;

    return LeadershipResult::granted(lease);
}

LeadershipResult
FakeLeadershipBackend::renew(const LeadershipLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_) return LeadershipResult::unavailable();

    auto it = leases_.find(lease.singleton_name);
    if (it == leases_.end() || !it->second.owned) {
        return LeadershipResult::lost();
    }
    if (it->second.lease.owner_node_id != lease.owner_node_id) {
        return LeadershipResult::lost();
    }

    // Bump token and extend deadline
    it->second.lease.fencing_token = next_token_++;
    it->second.lease.backend_revision = it->second.lease.fencing_token;
    it->second.lease.lease_deadline = Clock::now() +
        (lease.lease_deadline - Clock::now()); // same TTL
    return LeadershipResult::renewed(it->second.lease);
}

LeadershipResult
FakeLeadershipBackend::release(const LeadershipLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_) return LeadershipResult::unavailable();

    auto it = leases_.find(lease.singleton_name);
    if (it == leases_.end() || !it->second.owned) {
        return LeadershipResult::released(); // idempotent
    }
    if (it->second.lease.owner_node_id != lease.owner_node_id) {
        return LeadershipResult::lost(); // not our lease
    }

    it->second.owned = false;
    return LeadershipResult::released();
}

LeadershipResult
FakeLeadershipBackend::current_owner(std::string_view singleton_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unavailable_) return LeadershipResult::unavailable();

    std::string key(singleton_name);
    auto it = leases_.find(key);
    if (it == leases_.end() || !it->second.owned) {
        return {LeadershipStatusCode::NotOwner, std::nullopt, std::nullopt};
    }
    LeadershipResult r;
    r.status = LeadershipStatusCode::Granted;
    r.lease = it->second.lease;
    return r;
}

void FakeLeadershipBackend::force_grant(const std::string& singleton_name,
                                         const std::string& owner_node_id,
                                         Clock::duration ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    LeadershipLease lease;
    lease.singleton_name = singleton_name;
    lease.owner_node_id = owner_node_id;
    lease.fencing_token = next_token_++;
    lease.lease_deadline = Clock::now() + ttl;
    StoredLease stored;
    stored.lease = lease;
    stored.owned = true;
    leases_[singleton_name] = stored;
}

void FakeLeadershipBackend::force_revoke(const std::string& singleton_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    leases_.erase(singleton_name);
}

void FakeLeadershipBackend::simulate_unavailable(bool unavailable) {
    std::lock_guard<std::mutex> lock(mutex_);
    unavailable_ = unavailable;
}

int FakeLeadershipBackend::get_grant_count(const std::string& singleton_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = leases_.find(singleton_name);
    if (it == leases_.end()) return 0;
    return it->second.owned ? 1 : 0;
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 5: Add source to CMakeLists.txt**

```cmake
# In src/cluster/CMakeLists.txt, add after singleton/leadership_status.cpp:
    singleton/fake_leadership_backend.cpp
```

- [ ] **Step 6: Run test to verify it passes**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*FakeLeadershipBackend*"`
Expected: 9 tests PASS

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cluster/singleton/fake_leadership_backend.hpp \
        src/cluster/singleton/fake_leadership_backend.cpp \
        tests/unit/cluster/singleton/test_fake_leadership_backend.cpp \
        src/cluster/CMakeLists.txt tests/unit/cluster/CMakeLists.txt
git commit -m "feat(cluster): add FakeLeadershipBackend for deterministic testing"
```

---

### Task 6: LeadershipBackendAdapter (ISingletonElection impl)

**Files:**
- Create: `include/hpactor/cluster/singleton/leadership_backend_adapter.hpp`
- Create: `src/cluster/singleton/leadership_backend_adapter.cpp`
- Create: `tests/unit/cluster/singleton/test_leadership_backend_adapter.cpp`

**Interfaces:**
- Consumes: `ISingletonElection` (existing), `ILeadershipBackend` (Task 3), `FakeLeadershipBackend` (Task 5)
- Produces: `LeadershipBackendAdapter` — implements `ISingletonElection` by calling `ILeadershipBackend::try_acquire()`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/cluster/singleton/test_leadership_backend_adapter.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>
#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>

namespace hpactor::cluster::singleton {

using namespace std::chrono_literals;

class LeadershipBackendAdapterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        backend_ = std::make_unique<FakeLeadershipBackend>();
    }
    std::unique_ptr<FakeLeadershipBackend> backend_;
};

TEST_F(LeadershipBackendAdapterTest, ElectReturnsOwnerWhenBackendGrantsLease) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self", "node-other"};
    auto winner = adapter.elect(id, alive);

    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, "node-self");
}

TEST_F(LeadershipBackendAdapterTest, ElectReturnsOtherOwnerWhenNotSelf) {
    backend_->force_grant("shard-coordinator", "node-other", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self", "node-other"};
    auto winner = adapter.elect(id, alive);

    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, "node-other");
}

TEST_F(LeadershipBackendAdapterTest, ElectReturnsNulloptWhenNoOwner) {
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    auto winner = adapter.elect(id, alive);

    EXPECT_FALSE(winner.has_value());
}

TEST_F(LeadershipBackendAdapterTest, ElectReturnsNulloptWhenBackendUnavailable) {
    backend_->simulate_unavailable(true);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    auto winner = adapter.elect(id, alive);

    EXPECT_FALSE(winner.has_value());
}

TEST_F(LeadershipBackendAdapterTest, GetFencingTokenReturnsBackendToken) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    adapter.elect(id, alive);

    auto token = adapter.get_fencing_token("shard-coordinator");
    EXPECT_GT(token, 0u);
}

TEST_F(LeadershipBackendAdapterTest, GetFencingTokenReturnsZeroWhenNoLease) {
    LeadershipBackendAdapter adapter("node-self", backend_.get());
    EXPECT_EQ(adapter.get_fencing_token("shard-coordinator"), 0u);
}

TEST_F(LeadershipBackendAdapterTest, GetFencingTokenReturnsZeroForUnknownSingleton) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);
    LeadershipBackendAdapter adapter("node-self", backend_.get());

    SingletonIdentity id{"shard-coordinator", 0};
    std::vector<std::string> alive = {"node-self"};
    adapter.elect(id, alive);

    EXPECT_EQ(adapter.get_fencing_token("other-singleton"), 0u);
}

TEST_F(LeadershipBackendAdapterTest, OnPeerDownIsNoOp) {
    LeadershipBackendAdapter adapter("node-self", backend_.get());
    // Should not throw or crash — backend handles fencing
    adapter.on_peer_down("node-other");
    SUCCEED();
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*LeadershipBackendAdapter*"`
Expected: FAIL — `leadership_backend_adapter.hpp` not found

- [ ] **Step 3: Write the header**

```cpp
// include/hpactor/cluster/singleton/leadership_backend_adapter.hpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_backend.hpp>
#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

/// \brief Adapts an ILeadershipBackend to the ISingletonElection interface.
///
/// Each call to elect() internally calls ILeadershipBackend::try_acquire().
/// The adapter caches the latest lease per singleton and exposes the
/// backend-issued fencing token via get_fencing_token().
///
/// on_peer_down() is a no-op — the backend is the fencing authority.
class LeadershipBackendAdapter : public ISingletonElection {
  public:
    /// \param[in] self_node_id This node's identity.
    /// \param[in] backend The leadership backend (owned by caller).
    LeadershipBackendAdapter(std::string self_node_id,
                             ILeadershipBackend* backend);

    std::optional<std::string>
    elect(const SingletonIdentity& id,
          std::span<const std::string> alive_nodes) override;

    void on_peer_down(const std::string& node_id) override;

    /// \brief Return the backend-issued fencing token for a singleton.
    uint64_t get_fencing_token(std::string_view singleton_name) const override;

  private:
    std::string self_node_id_;
    ILeadershipBackend* backend_;
    std::unordered_map<std::string, LeadershipLease> leases_;
};

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/cluster/singleton/leadership_backend_adapter.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>

namespace hpactor::cluster::singleton {

LeadershipBackendAdapter::LeadershipBackendAdapter(std::string self_node_id,
                                                   ILeadershipBackend* backend)
    : self_node_id_(std::move(self_node_id)), backend_(backend) {}

std::optional<std::string>
LeadershipBackendAdapter::elect(const SingletonIdentity& id,
                                std::span<const std::string> /*alive_nodes*/) {
    // alive_nodes is not used — backend owns the decision
    LeadershipAttempt attempt;
    attempt.singleton_name = id.name;
    attempt.self_node_id = self_node_id_;
    attempt.lease_ttl = std::chrono::seconds(10); // default TTL, configurable later

    auto result = backend_->try_acquire(attempt);

    if (result.status == LeadershipStatusCode::Granted && result.lease.has_value()) {
        leases_[id.name] = *result.lease;
        return result.lease->owner_node_id;
    }

    if (result.status == LeadershipStatusCode::AlreadyOwned) {
        if (result.lease.has_value()) {
            leases_[id.name] = *result.lease;
        }
        return result.current_owner;
    }

    if (result.status == LeadershipStatusCode::Renewed && result.lease.has_value()) {
        leases_[id.name] = *result.lease;
        return result.lease->owner_node_id;
    }

    // Backend unavailable, lost, or no owner — no winner
    return std::nullopt;
}

void LeadershipBackendAdapter::on_peer_down(const std::string& /*node_id*/) {
    // Backend is the fencing authority — no local vote cleanup needed
}

uint64_t
LeadershipBackendAdapter::get_fencing_token(std::string_view singleton_name) const {
    std::string key(singleton_name);
    auto it = leases_.find(key);
    if (it == leases_.end()) return 0;
    return it->second.fencing_token;
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 5: Add source to CMakeLists.txt**

```cmake
# In src/cluster/CMakeLists.txt, add after singleton/fake_leadership_backend.cpp:
    singleton/leadership_backend_adapter.cpp
```

- [ ] **Step 6: Run test to verify it passes**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*LeadershipBackendAdapter*"`
Expected: 8 tests PASS

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cluster/singleton/leadership_backend_adapter.hpp \
        src/cluster/singleton/leadership_backend_adapter.cpp \
        tests/unit/cluster/singleton/test_leadership_backend_adapter.cpp \
        src/cluster/CMakeLists.txt tests/unit/cluster/CMakeLists.txt
git commit -m "feat(cluster): add LeadershipBackendAdapter — ISingletonElection over ILeadershipBackend"
```

---

### Task 7: Update SingletonManagerCore to use backend fencing token

**Files:**
- Modify: `src/cluster/singleton/singleton_manager.cpp` (line 47: replace local increment with backend token query)
- Create: `tests/unit/cluster/singleton/test_singleton_manager_leader.cpp`

**Interfaces:**
- Consumes: `ISingletonElection::get_fencing_token()` (Task 4), `LeadershipBackendAdapter` (Task 6)
- Produces: Backward-compatible `SingletonManagerCore` that sources fencing token from election strategy

- [ ] **Step 1: Write the failing test for adapter-backed singleton manager**

```cpp
// tests/unit/cluster/singleton/test_singleton_manager_leader.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>
#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>
#include <hpactor/cluster/singleton/singleton_manager.hpp>

namespace hpactor::cluster::singleton {

using namespace std::chrono_literals;

class SingletonManagerLeaderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        backend_ = std::make_unique<FakeLeadershipBackend>();
    }

    /// \brief Create a SingletonManagerCore backed by the fake backend.
    SingletonManagerCore make_manager(const std::string& node_id) {
        auto adapter = std::make_unique<LeadershipBackendAdapter>(node_id, backend_.get());
        return SingletonManagerCore(node_id, std::move(adapter));
    }

    std::unique_ptr<FakeLeadershipBackend> backend_;
};

TEST_F(SingletonManagerLeaderTest, BackendGrantActivatesSingleton) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);

    // Re-create with the backend that has a pre-granted lease
    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self"});

    EXPECT_EQ(mgr.get_state("shard-coordinator"), SingletonState::Active);
}

TEST_F(SingletonManagerLeaderTest, FencingTokenFromBackend) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);

    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self"});

    auto token = mgr.get_fencing_token("shard-coordinator");
    EXPECT_GT(token, 0u);
}

TEST_F(SingletonManagerLeaderTest, OtherOwnerKeepsSelfInStandby) {
    backend_->force_grant("shard-coordinator", "node-other", 10s);

    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self", "node-other"});

    EXPECT_EQ(mgr.get_state("shard-coordinator"), SingletonState::Standby);
}

TEST_F(SingletonManagerLeaderTest, BackendUnavailableKeepsStandby) {
    backend_->simulate_unavailable(true);

    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self"});

    EXPECT_EQ(mgr.get_state("shard-coordinator"), SingletonState::Standby);
}

TEST_F(SingletonManagerLeaderTest, ExistingLocalElectionTestsStillPass) {
    // Verify that OldestNodeElection still works unchanged
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerCore mgr("node-self", std::move(election));
    mgr.register_singleton(SingletonIdentity{"test-singleton", 0});
    mgr.on_node_state_change({"node-self"});

    EXPECT_EQ(mgr.get_state("test-singleton"), SingletonState::Active);
    // Local election increments token locally (get_fencing_token returns 0)
    auto token = mgr.get_fencing_token("test-singleton");
    EXPECT_GT(token, 0u); // locally incremented to 1
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*SingletonManagerLeader*"`
Expected: FAIL — backend token not flowing to SingletonIdentity

- [ ] **Step 3: Modify SingletonManagerCore to use backend token**

In `src/cluster/singleton/singleton_manager.cpp`, modify `on_node_state_change()`:

Replace lines 40-48:
```cpp
        if (winner.has_value() && *winner == self_node_) {
            // Self won — activate if not already active
            if (record.state == SingletonState::Standby) {
                record.state = SingletonState::Activating;
            }
            if (record.state == SingletonState::Activating) {
                record.state = SingletonState::Active;
                record.identity.fencing_token++;
            }
```

With:
```cpp
        if (winner.has_value() && *winner == self_node_) {
            // Self won — activate if not already active
            if (record.state == SingletonState::Standby) {
                record.state = SingletonState::Activating;
            }
            if (record.state == SingletonState::Activating) {
                record.state = SingletonState::Active;
                // Prefer backend-issued token; fall back to local increment
                uint64_t backend_token = election_->get_fencing_token(name);
                if (backend_token > 0) {
                    record.identity.fencing_token = backend_token;
                } else {
                    record.identity.fencing_token++;
                }
            }
```

- [ ] **Step 4: Run ALL existing singleton tests to verify no regression**

Run: `ctest -R "Singleton|Election" --output-on-failure`
Expected: All existing tests PASS (both local and backend paths)

- [ ] **Step 5: Run new adapter-backed tests**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*SingletonManagerLeader*"`
Expected: 5 tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/cluster/singleton/singleton_manager.cpp \
        tests/unit/cluster/singleton/test_singleton_manager_leader.cpp \
        tests/unit/cluster/CMakeLists.txt
git commit -m "feat(cluster): source fencing token from election strategy in SingletonManagerCore"
```

---

### Task 8: TOML config parser for [system.cluster.leadership]

**Files:**
- Create: `src/config/parsers/cluster_leadership_parser.cpp`
- Modify: `src/cluster/CMakeLists.txt` (ensure parser compiles into hpactor_cluster — or create a dedicated parser registration)

**Interfaces:**
- Consumes: `TomlTableView`, `TomlSystemParserRegistration<T>`
- Produces: Parsed `[system.cluster.leadership]` section — `LeadershipConfig` struct consumed by `ClusterRuntimeImpl`

- [ ] **Step 1: Check existing parser pattern for reference**

Read: `src/config/parsers/` — find an existing self-registering parser and follow the same pattern.

- [ ] **Step 2: Define LeadershipConfig and write the parser**

```cpp
// src/config/parsers/cluster_leadership_parser.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/config/toml_table_view.hpp>
#include <hpactor/config/toml_parser_registry.hpp>

#include <string>

namespace hpactor::config {
namespace {

struct LeadershipConfig {
    std::string mode = "local";       // "local" | "external" | "disabled"
    std::string backend = "etcd";     // "etcd" | "consul" | "raft"
    uint32_t lease_ttl_ms = 10000;
    uint32_t renew_interval_ms = 3000;
    uint32_t renew_deadline_ms = 7000;
    uint32_t step_down_grace_ms = 1000;
    bool fail_closed = true;
};

LeadershipConfig parse_leadership_config(const TomlTableView& cluster_table) {
    LeadershipConfig cfg;

    auto leadership = cluster_table.get_table("leadership");
    if (!leadership) return cfg;

    cfg.mode = leadership->get_string("mode").value_or("local");
    cfg.backend = leadership->get_string("backend").value_or("etcd");
    cfg.lease_ttl_ms = leadership->get_uint("lease_ttl_ms").value_or(10000);
    cfg.renew_interval_ms = leadership->get_uint("renew_interval_ms").value_or(3000);
    cfg.renew_deadline_ms = leadership->get_uint("renew_deadline_ms").value_or(7000);
    cfg.step_down_grace_ms = leadership->get_uint("step_down_grace_ms").value_or(1000);
    cfg.fail_closed = leadership->get_bool("fail_closed_on_backend_unavailable").value_or(true);

    return cfg;
}

// Self-registering parser for the cluster subsystem
struct ClusterLeadershipParser {
    static void parse(const TomlTableView& cluster_table, void* /*context*/) {
        auto cfg = parse_leadership_config(cluster_table);
        // Config is stored for later retrieval by ClusterRuntimeImpl.
        // In the initial implementation, ClusterRuntimeImpl reads this
        // directly from TOML during start(). The stored config is
        // registered here for Phase 2 dynamic reload.
        (void)cfg; // consumed in ClusterRuntimeImpl
    }
};

// Register parser — called automatically at static init time
static TomlSystemParserRegistration<ClusterLeadershipParser>
    leadership_reg("cluster");

} // namespace
} // namespace hpactor::config
```

- [ ] **Step 3: Build to verify compilation**

Run: `ninja -C build`
Expected: Compiles clean

- [ ] **Step 4: Commit**

```bash
git add src/config/parsers/cluster_leadership_parser.cpp
git commit -m "feat(config): add TOML parser for [system.cluster.leadership]"
```

---

### Task 9: Wire into ClusterRuntimeImpl — config-driven election strategy

**Files:**
- Modify: `src/cluster/cluster_runtime_impl.cpp` (lines 53-54: replace hardcoded OldestNodeElection with config-driven choice)
- Modify: `src/cluster/cluster_runtime_impl.hpp` (store leadership mode)

**Interfaces:**
- Consumes: `LeadershipBackendAdapter` (Task 6), `FakeLeadershipBackend` (Task 5), `ILeadershipBackend` (Task 3)
- Produces: `ClusterRuntimeImpl` creates `LeadershipBackendAdapter` + `FakeLeadershipBackend` when `mode="external"` (fake backend for tests), or `OldestNodeElection` when `mode="local"`

- [ ] **Step 1: Modify ClusterRuntimeImpl::start()**

The initial implementation uses a compile-time or simple config flag. In `cluster_runtime_impl.cpp`, change lines 53-54:

```cpp
// Before:
singleton_manager_ = std::make_unique<cluster::singleton::SingletonManagerActor>(
    node_id_, std::make_unique<cluster::singleton::OldestNodeElection>());

// After (with leadership config support):
// For now, use mode="local" default. Phase 2 etcd backend will read TOML.
// The fake backend path is available for integration tests.
auto election = std::make_unique<cluster::singleton::OldestNodeElection>();
singleton_manager_ = std::make_unique<cluster::singleton::SingletonManagerActor>(
    node_id_, std::move(election));
```

(Full config-driven wiring arrives with the etcd backend in M2. For M1, the adapter path is tested directly via `SingletonManagerCore` + `LeadershipBackendAdapter` in unit tests.)

- [ ] **Step 2: Run full cluster test suite to verify no regression**

Run: `ctest -R "cluster|Cluster|Singleton|Election" --output-on-failure`
Expected: All 98+ cluster tests PASS

- [ ] **Step 3: Commit**

```bash
git add src/cluster/cluster_runtime_impl.cpp
git commit -m "feat(cluster): prepare ClusterRuntimeImpl for config-driven election strategy"
```

---

### Task 10: M1 Integration verification

- [ ] **Step 1: Build everything from clean**

Run: `ninja -C build`
Expected: Zero errors, zero warnings

- [ ] **Step 2: Run full test suite**

Run: `ctest --output-on-failure --parallel 8`
Expected: All tests PASS (no regressions)

- [ ] **Step 3: Verify key invariant — all existing tests unchanged**

Run: `ctest -R "Singleton|Election|cluster|Cluster" --output-on-failure`
Expected: All existing cluster tests PASS with local election still default

- [ ] **Step 4: Commit M1 final**

```bash
git commit --allow-empty -m "feat(cluster): M1 complete — LeadershipLease, ILeadershipBackend, FakeBackend, Adapter"
```

---

## M2: etcd Backend + Shard Coordinator Gating (Outline)

**Goal:** Real external coordinator production path. ShardCoordinatorActor rejects mutations without valid token.

### Task 11: EtcdLeadershipBackend
- Create: `include/hpactor/cluster/singleton/etcd_leadership_backend.hpp`
- Create: `src/cluster/singleton/etcd_leadership_backend.cpp`
- Uses bounded blocking executor (not in scheduler hot path)
- Implements: acquire (etcd lease + txn), renew (keepalive), release (delete + revoke), watch (etcd watch with compaction recovery)
- Tests: integration tests against embedded etcd or testcontainer

### Task 12: TLS config wiring
- Read etcd TLS settings from `[system.cluster.leadership.etcd]` TOML section
- Cert file paths, request timeout, endpoints

### Task 13: ShardCoordinatorCore token validation
- Modify: `include/hpactor/cluster/sharding/shard_coordinator.hpp`
- Add `set_active_lease(LeadershipLease)`, `validate_token(fencing_token, singleton_name) -> bool`
- Reject: no token, stale token, wrong singleton, missing lease

### Task 14: ShardCoordinatorActor integration
- Modify: `src/cluster/sharding/shard_coordinator_actor.cpp`
- Accept lease from `SingletonManagerActor`, pass to core

### Task 15: ConsulLeadershipBackend (optional)
- Create adapter with session + KV lock pattern
- Only if deployment demand exists

---
## M3: Internal Raft Backend (Future Outline)

**Goal:** In-process Raft backend implementing same ILeadershipBackend contract.

Refer to `docs/architecture/production/internal-raft-leadership-backend-design.md` for detailed design.

### Task 16: RaftNodeCore
- Deterministic state machine: follower/candidate/leader, term, vote, log matching, commit index
- PreVote, CheckQuorum, Leader Transfer

### Task 17: RaftLogStore + RaftSnapshotStore
- Durable append-only log with CRC, atomic segment replacement
- Snapshots with compaction

### Task 18: RaftTransport
- Bounded control-plane RPC for AppendEntries, RequestVote, InstallSnapshot

### Task 19: RaftLeadershipBackend adapter
- Implements ILeadershipBackend by proposing GrantLeadership/RenewLeadership/ReleaseLeadership commands to Raft
- Fencing token from committed (term << 32) | log_index

### Task 20: Partition/chaos/soak tests
- Deterministic simulation: message reorder/drop/duplicate
- Split 3/2, leader isolation, crash recovery, snapshot install
- Joint consensus membership change validation
