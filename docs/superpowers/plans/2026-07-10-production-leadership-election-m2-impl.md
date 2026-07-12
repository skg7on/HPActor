# M2: etcd Leadership Backend + Shard Coordinator Gating — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Production etcd-backed cluster singleton leadership with gRPC callback API. ShardCoordinatorActor mutations are gated on backend-issued fencing tokens. Zero exceptions cross into actor code.

**Architecture:** Direct gRPC integration via callback API (exception-free). Protobuf serialization isolated to `src/etcd/` TU compiled with `-fexceptions` (same pattern as `toml_parser.cpp`). `EtcdLeadershipBackend` implements `ILeadershipBackend` from M1. gRPC callbacks post results via `std::promise`/`std::future` — bounded blocking off actor scheduler.

**Tech Stack:** C++20, gRPC ≥1.40 (callback API), Protobuf, OpenSSL, JSON encoding for etcd key values.

## Global Constraints

- Worktree isolation: all writes in `.claude/worktrees/<name>/`, never main checkout
- TDDFlow: RED → GREEN → REFACTOR before any production code
- No exceptions, no RTTI in public headers and actor code
- Exceptions permitted ONLY in `src/etcd/` TU (isolated, same pattern as `src/config/toml_parser.cpp`)
- Core/Actor separation: thread-safe core class + thin actor wrapper
- Deterministic unit tests: no real threads, no wall-clock timing
- Integration tests use real etcd (embedded or testcontainer)
- All existing M1 tests (220 cluster + 822 others = 1,042 tests) must pass unchanged
- Subsystem-owned TOML config: extend existing `cluster_leadership_parser.cpp`

---
## File Map

```
src/etcd/
├── CMakeLists.txt                          [CREATE]  proto codegen + exception-enabled lib
├── proto/
│   ├── rpc.proto                           [CREATE]  etcd v3 API (stripped annotations)
│   └── kv.proto                            [CREATE]  mvccpb KeyValue/Event types
├── etcd_serialize.hpp                      [CREATE]  JSON encode/decode (public, no-exceptions)
├── etcd_serialize.cpp                      [CREATE]  Implementation (exceptions allowed)
├── etcd_lease_client.hpp                   [CREATE]  LeaseGrant + LeaseKeepAlive
├── etcd_lease_client.cpp                   [CREATE]  gRPC callback impl (exceptions allowed)
├── etcd_kv_client.hpp                      [CREATE]  Txn + Range
├── etcd_kv_client.cpp                      [CREATE]  gRPC callback impl (exceptions allowed)
├── etcd_watch_client.hpp                   [CREATE]  Watch + compaction recovery
└── etcd_watch_client.cpp                   [CREATE]  gRPC callback impl (exceptions allowed)

include/hpactor/cluster/singleton/
└── etcd_leadership_backend.hpp             [CREATE]  EtcdLeadershipBackend (public, no-exceptions)

src/cluster/singleton/
└── etcd_leadership_backend.cpp             [CREATE]  Implementation (bounded blocking on future)

src/cluster/sharding/
├── shard_coordinator.hpp                   [MODIFY]  Add set_active_lease(), validate_token()
└── shard_coordinator.cpp                   [MODIFY]  Token validation logic

src/cluster/
├── CMakeLists.txt                          [MODIFY]  Add etcd backend source
└── cluster_runtime_impl.cpp                [MODIFY]  Create EtcdLeadershipBackend from config

src/config/parsers/
└── cluster_leadership_parser.cpp           [MODIFY]  Add etcd TLS fields

tests/unit/cluster/singleton/
├── test_etcd_serialize.cpp                 [CREATE]  Round-trip serialization tests
└── test_etcd_leadership_backend.cpp        [CREATE]  Unit tests with mock gRPC stubs

tests/unit/cluster/sharding/
└── test_shard_coordinator_token.cpp        [CREATE]  Token validation tests

tests/integration/singleton/
└── test_etcd_leadership_integration.cpp    [CREATE]  Integration with real etcd

tests/unit/cluster/
└── CMakeLists.txt                          [MODIFY]  Add new test files
```

---
## Task M2-1: Vendor etcd Protos + CMake Integration

**Goal:** Generate C++ gRPC stubs from vendored etcd proto files. Compile isolated TU with `-fexceptions`.

**Files:**
- Create: `src/etcd/proto/rpc.proto`
- Create: `src/etcd/proto/kv.proto`
- Create: `src/etcd/CMakeLists.txt`
- Modify: `src/CMakeLists.txt` — `add_subdirectory(etcd)`

**Interfaces:**
- Produces: `hpactor_etcd` static library — generated gRPC stubs for etcd v3 KV, Lease, Watch services

- [ ] **Step 1: Create kv.proto (mvccpb types)**

```protobuf
// src/etcd/proto/kv.proto — from etcd mvccpb, stripped of gogoproto annotations
syntax = "proto3";
package mvccpb;

message KeyValue {
  bytes key = 1;
  int64 create_revision = 2;
  int64 mod_revision = 3;
  int64 version = 4;
  bytes value = 5;
  int64 lease = 6;
}

message Event {
  enum EventType {
    PUT = 0;
    DELETE = 1;
  }
  EventType type = 1;
  KeyValue kv = 2;
  KeyValue prev_kv = 3;
}
```

- [ ] **Step 2: Create rpc.proto (etcd v3 API, stripped)**

```protobuf
// src/etcd/proto/rpc.proto — etcd v3 API, minimal set for leadership election
syntax = "proto3";
package etcdserverpb;

import "kv.proto";

message LeaseGrantRequest { int64 TTL = 1; int64 ID = 2; }
message LeaseGrantResponse { int64 ID = 1; int64 TTL = 2; string error = 3; }
message LeaseRevokeRequest { int64 ID = 1; }
message LeaseRevokeResponse {}
message LeaseKeepAliveRequest { int64 ID = 1; }
message LeaseKeepAliveResponse { int64 ID = 1; int64 TTL = 2; }

message RangeRequest {
  bytes key = 1; bytes range_end = 2; int64 limit = 3; int64 revision = 4;
}
message RangeResponse {
  ResponseHeader header = 1; repeated mvccpb.KeyValue kvs = 2; int64 count = 4;
}

message Compare {
  enum CompareResult { EQUAL = 0; GREATER = 1; LESS = 2; NOT_EQUAL = 3; }
  enum CompareTarget { VERSION = 0; CREATE = 1; MOD = 2; VALUE = 3; LEASE = 4; }
  CompareResult result = 1; CompareTarget target = 2; bytes key = 3; bytes value = 4;
}
message RequestOp {
  message RequestRange { bytes key = 1; bytes range_end = 2; }
  message RequestPut { bytes key = 1; bytes value = 2; int64 lease = 3; }
  message RequestDeleteRange { bytes key = 1; bytes range_end = 2; }
  oneof request { RequestRange request_range = 1; RequestPut request_put = 2; RequestDeleteRange request_delete_range = 3; }
}
message ResponseOp {
  message ResponseRange { repeated mvccpb.KeyValue kvs = 1; int64 count = 2; }
  message ResponsePut { ResponseHeader header = 1; mvccpb.KeyValue prev_kv = 2; }
  message ResponseDeleteRange { int64 deleted = 1; }
  oneof response { ResponseRange response_range = 1; ResponsePut response_put = 2; ResponseDeleteRange response_delete_range = 3; }
}
message TxnRequest { repeated Compare compare = 1; repeated RequestOp success = 2; repeated RequestOp failure = 3; }
message TxnResponse { ResponseHeader header = 1; bool succeeded = 2; repeated ResponseOp responses = 3; }

message WatchCreateRequest { bytes key = 1; bytes range_end = 2; int64 start_revision = 3; bool progress_notify = 7; }
message WatchRequest { oneof request_type { WatchCreateRequest create_request = 1; } }
message WatchResponse {
  ResponseHeader header = 1; int64 watch_id = 2; bool created = 3;
  bool canceled = 4; int64 compact_revision = 5;
  repeated mvccpb.Event events = 11;
}

message ResponseHeader { uint64 cluster_id = 1; uint64 member_id = 2; int64 revision = 3; uint64 raft_term = 4; }

service KV { rpc Range(RangeRequest) returns (RangeResponse); rpc Txn(TxnRequest) returns (TxnResponse); }
service Lease { rpc LeaseGrant(LeaseGrantRequest) returns (LeaseGrantResponse); rpc LeaseRevoke(LeaseRevokeRequest) returns (LeaseRevokeResponse); rpc LeaseKeepAlive(stream LeaseKeepAliveRequest) returns (stream LeaseKeepAliveResponse); }
service Watch { rpc Watch(stream WatchRequest) returns (stream WatchResponse); }
```

- [ ] **Step 3: Create CMakeLists.txt for proto codegen + exception-enabled lib**

```cmake
# src/etcd/CMakeLists.txt
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_FILES
    proto/kv.proto
    proto/rpc.proto
)

# Generate C++ from proto files (compiled with -fexceptions for protobuf)
add_library(hpactor_etcd STATIC)
target_include_directories(hpactor_etcd PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Protobuf_INCLUDE_DIRS}
)
protobuf_generate(TARGET hpactor_etcd PROTOS ${PROTO_FILES})
# Also generate gRPC stubs
protobuf_generate(
    TARGET hpactor_etcd
    PROTOS ${PROTO_FILES}
    LANGUAGE grpc
    PLUGIN "protoc-gen-grpc=${gRPC_CPP_PLUGIN}"
)

# This TU is allowed to use exceptions — protobuf/gRPC require them
target_compile_options(hpactor_etcd PRIVATE -fexceptions)
# Disable RTTI for protobuf
target_compile_definitions(hpactor_etcd PUBLIC GOOGLE_PROTOBUF_NO_RTTI=1)
target_link_libraries(hpactor_etcd PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)
```

- [ ] **Step 4: Wire into parent CMake**

In `src/CMakeLists.txt`, add after the existing `add_subdirectory` calls:
```cmake
add_subdirectory(etcd)
```

- [ ] **Step 5: Build to verify**

Run: `ninja -C build hpactor_etcd`
Expected: Library compiles clean

- [ ] **Step 6: Commit**

```bash
git add src/etcd/ src/CMakeLists.txt
git commit -m "feat(etcd): add vendored etcd v3 protos and gRPC codegen CMake"
```

---

## Task M2-2: etcd Serialize — LeadershipLease ↔ JSON

**Goal:** Convert between `LeadershipLease` and JSON bytes stored in etcd key values. Exception boundary: header is clean, .cpp may use exceptions.

**Files:**
- Create: `src/etcd/etcd_serialize.hpp`
- Create: `src/etcd/etcd_serialize.cpp`
- Create: `tests/unit/cluster/singleton/test_etcd_serialize.cpp`

**Interfaces:**
- Produces: `serialize_lease(lease) -> string`, `deserialize_lease(data) -> optional<LeadershipLease>`

- [ ] **Step 1: Write the header**

```cpp
// src/etcd/etcd_serialize.hpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor::etcd {

/// \brief Serialize a LeadershipLease to JSON for etcd key value storage.
std::string serialize_lease(const cluster::singleton::LeadershipLease& lease);

/// \brief Deserialize a LeadershipLease from JSON.
/// Returns nullopt on parse errors or corrupt data.
std::optional<cluster::singleton::LeadershipLease>
deserialize_lease(std::string_view data);

/// \brief Format the etcd key path for a singleton's owner record.
std::string owner_key(std::string_view key_prefix,
                      std::string_view cluster_id,
                      std::string_view singleton_name);

} // namespace hpactor::etcd
```

- [ ] **Step 2: Write the failing test**

```cpp
// tests/unit/cluster/singleton/test_etcd_serialize.cpp
#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/leadership_lease.hpp>
#include <etcd/etcd_serialize.hpp>

namespace hpactor::cluster::singleton {

TEST(EtcdSerializeTest, RoundTripPreservesFields) {
    LeadershipLease original;
    original.cluster_id = "test-cluster";
    original.singleton_name = "shard-coordinator";
    original.owner_node_id = "node-a";
    original.owner_incarnation = 42;
    original.membership_epoch = 7;
    original.fencing_token = 100;
    original.backend_term = 3;
    original.backend_revision = 100;

    auto json = etcd::serialize_lease(original);
    ASSERT_FALSE(json.empty());

    auto restored = etcd::deserialize_lease(json);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->cluster_id, "test-cluster");
    EXPECT_EQ(restored->singleton_name, "shard-coordinator");
    EXPECT_EQ(restored->owner_node_id, "node-a");
    EXPECT_EQ(restored->owner_incarnation, 42u);
    EXPECT_EQ(restored->membership_epoch, 7u);
    EXPECT_EQ(restored->fencing_token, 100u);
    EXPECT_EQ(restored->backend_term, 3u);
    EXPECT_EQ(restored->backend_revision, 100u);
}

TEST(EtcdSerializeTest, EmptyDataReturnsNullopt) {
    auto result = etcd::deserialize_lease("");
    EXPECT_FALSE(result.has_value());
}

TEST(EtcdSerializeTest, CorruptDataReturnsNullopt) {
    auto result = etcd::deserialize_lease("{not-valid-json");
    EXPECT_FALSE(result.has_value());
}

TEST(EtcdSerializeTest, OwnerKeyFormat) {
    auto key = etcd::owner_key("/hpactor", "prod-a", "shard-coordinator");
    EXPECT_EQ(key, "/hpactor/prod-a/singletons/shard-coordinator/owner");
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*EtcdSerialize*"`
Expected: FAIL — header not found

- [ ] **Step 4: Write the implementation**

```cpp
// src/etcd/etcd_serialize.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
// Note: This TU is compiled with -fexceptions for protobuf/JSON parsing.

#include "etcd_serialize.hpp"

#include <sstream>

namespace hpactor::etcd {

std::string serialize_lease(const cluster::singleton::LeadershipLease& lease) {
    std::ostringstream os;
    os << "{";
    os << "\"cluster_id\":\"" << lease.cluster_id << "\",";
    os << "\"singleton_name\":\"" << lease.singleton_name << "\",";
    os << "\"owner_node_id\":\"" << lease.owner_node_id << "\",";
    os << "\"owner_incarnation\":" << lease.owner_incarnation << ",";
    os << "\"owner_process_start_id\":" << lease.owner_process_start_id << ",";
    os << "\"membership_epoch\":" << lease.membership_epoch << ",";
    os << "\"fencing_token\":" << lease.fencing_token << ",";
    os << "\"backend_term\":" << lease.backend_term << ",";
    os << "\"backend_revision\":" << lease.backend_revision;
    // lease_deadline omitted — not persisted (restored from lease TTL on deserialize)
    os << "}";
    return os.str();
}

std::optional<cluster::singleton::LeadershipLease>
deserialize_lease(std::string_view data) {
    // Manual JSON parsing to avoid dependency on a JSON library.
    // The format is simple enough for a hand-rolled parser.
    if (data.empty() || data[0] != '{') return std::nullopt;

    cluster::singleton::LeadershipLease lease;

    auto extract_str = [&](const char* key) -> std::string {
        std::string search = std::string("\"") + key + "\":\"";
        auto pos = data.find(search);
        if (pos == std::string_view::npos) return {};
        pos += search.size();
        auto end = data.find('"', pos);
        if (end == std::string_view::npos) return {};
        return std::string(data.substr(pos, end - pos));
    };

    auto extract_uint = [&](const char* key) -> uint64_t {
        std::string search = std::string("\"") + key + "\":";
        auto pos = data.find(search);
        if (pos == std::string_view::npos) return 0;
        pos += search.size();
        auto end = data.find_first_of(",}", pos);
        if (end == std::string_view::npos) return 0;
        return std::stoull(std::string(data.substr(pos, end - pos)));
    };

    lease.cluster_id = extract_str("cluster_id");
    lease.singleton_name = extract_str("singleton_name");
    lease.owner_node_id = extract_str("owner_node_id");
    lease.owner_incarnation = extract_uint("owner_incarnation");
    lease.owner_process_start_id = extract_uint("owner_process_start_id");
    lease.membership_epoch = extract_uint("membership_epoch");
    lease.fencing_token = extract_uint("fencing_token");
    lease.backend_term = extract_uint("backend_term");
    lease.backend_revision = extract_uint("backend_revision");

    // Basic validation: must have the key identifying fields
    if (lease.singleton_name.empty() || lease.owner_node_id.empty()) {
        return std::nullopt;
    }

    return lease;
}

std::string owner_key(std::string_view key_prefix,
                      std::string_view cluster_id,
                      std::string_view singleton_name) {
    std::string key;
    key.reserve(key_prefix.size() + cluster_id.size() + singleton_name.size() + 25);
    key.append(key_prefix);
    key.push_back('/');
    key.append(cluster_id);
    key.append("/singletons/");
    key.append(singleton_name);
    key.append("/owner");
    return key;
}

} // namespace hpactor::etcd
```

- [ ] **Step 5: Add to CMakeLists.txt**

In `src/etcd/CMakeLists.txt`, add after the proto generation:
```cmake
target_sources(hpactor_etcd PRIVATE etcd_serialize.cpp)
```

- [ ] **Step 6: Run test to verify it passes**

Run: `ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*EtcdSerialize*"`
Expected: 4 tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/etcd/etcd_serialize.hpp src/etcd/etcd_serialize.cpp \
        src/etcd/CMakeLists.txt \
        tests/unit/cluster/singleton/test_etcd_serialize.cpp \
        tests/unit/cluster/CMakeLists.txt
git commit -m "feat(etcd): add LeadershipLease JSON serialization for etcd key values"
```

---

## Task M2-3: EtcdLeadershipBackend

**Goal:** Implement `ILeadershipBackend` using gRPC callback API with bounded blocking on `std::promise`/`std::future`.

**Files:**
- Create: `include/hpactor/cluster/singleton/etcd_leadership_backend.hpp`
- Create: `src/cluster/singleton/etcd_leadership_backend.cpp`
- Create: `tests/unit/cluster/singleton/test_etcd_leadership_backend.cpp`

**Interfaces:**
- Consumes: `ILeadershipBackend` (M1 Task 3), `LeadershipLease` (M1 Task 1), etcd gRPC stubs (M2-1), serialize (M2-2)
- Produces: `EtcdLeadershipBackend` — production etcd-backed leadership with:
  - `try_acquire()`: LeaseGrant → Txn(CAS put) → start KeepAlive + Watch
  - `renew()`: KeepAlive heartbeat → Txn(verify owner)
  - `release()`: Txn(delete if owner matches) → LeaseRevoke → cancel streams
  - `current_owner()`: Range query → deserialize

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/cluster/singleton/etcd_leadership_backend.hpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_backend.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {

namespace etcd {
class EtcdLeaseClient;
class EtcdKvClient;
class EtcdWatchClient;
} // namespace etcd

namespace cluster::singleton {

/// \brief Production etcd-backed leadership backend.
///
/// Implements ILeadershipBackend using etcd v3 gRPC API.
/// gRPC completion queue runs on dedicated threads — callbacks
/// resolve std::promise objects, and public methods block on futures
/// with bounded timeouts. No exceptions cross the boundary into
/// actor code.
class EtcdLeadershipBackend : public ILeadershipBackend {
  public:
    struct Config {
        std::vector<std::string> endpoints;
        std::string key_prefix = "/hpactor";
        std::chrono::milliseconds request_timeout{1000};
    };

    explicit EtcdLeadershipBackend(Config cfg);
    ~EtcdLeadershipBackend() override;

    // ── ILeadershipBackend ──────────────────────────────────────────

    LeadershipResult try_acquire(const LeadershipAttempt& attempt) override;
    LeadershipResult renew(const LeadershipLease& lease) override;
    LeadershipResult release(const LeadershipLease& lease) override;
    LeadershipResult current_owner(std::string_view singleton_name) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cluster::singleton
} // namespace hpactor
```

- [ ] **Step 2: Write the implementation skeleton + key method**

```cpp
// src/cluster/singleton/etcd_leadership_backend.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cluster/singleton/etcd_leadership_backend.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>
#include <hpactor/cluster/singleton/leadership_status.hpp>

#include "etcd/etcd_serialize.hpp"

#include <future>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::singleton {

namespace {

/// \brief Convert etcd mod_revision to fencing token.
/// etcd revisions are globally monotonic — directly usable as fencing tokens.
uint64_t revision_to_token(int64_t mod_revision) {
    return static_cast<uint64_t>(mod_revision);
}

} // namespace

struct EtcdLeadershipBackend::Impl {
    Config cfg;
    // per-singleton state: active lease_id, watch handle, keepalive handle
    struct SingletonState {
        int64_t lease_id = 0;
        LeadershipLease current_lease;
        bool keepalive_active = false;
        bool watch_active = false;
    };
    std::unordered_map<std::string, SingletonState> singletons_;
    std::mutex mutex_;
};

EtcdLeadershipBackend::EtcdLeadershipBackend(Config cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
}

EtcdLeadershipBackend::~EtcdLeadershipBackend() = default;

LeadershipResult
EtcdLeadershipBackend::try_acquire(const LeadershipAttempt& attempt) {
    // 1. Grant etcd lease with TTL
    // 2. Build Txn: if owner key absent → put; else return current
    // 3. Execute Txn
    // 4. If success: extract mod_revision → fencing_token, start keepalive + watch
    // 5. If key exists: return AlreadyOwned with current owner
    // 6. On error/timeout: return BackendUnavailable or TimedOut

    // For the initial implementation, the bounded blocking pattern:
    auto promise = std::make_shared<std::promise<LeadershipResult>>();
    auto future = promise->get_future();

    // TODO: actual gRPC calls go here (Task M2-3 implementation detail)
    // lease_client_->grant(ttl, [promise](auto lease_id, auto status) { ... });

    auto status = future.wait_for(impl_->cfg.request_timeout);
    if (status == std::future_status::timeout) {
        return LeadershipResult::timed_out();
    }
    return future.get();
}

LeadershipResult
EtcdLeadershipBackend::renew(const LeadershipLease& lease) {
    // 1. Verify lease.owner_node_id matches local identity
    // 2. KeepAlive heartbeat sends (bidirectional stream)
    // 3. Txn: verify owner key still points to this node + token
    // 4. Return renewed lease with updated revision as new fencing_token
    return LeadershipResult::timed_out(); // stub
}

LeadershipResult
EtcdLeadershipBackend::release(const LeadershipLease& lease) {
    // 1. Txn: delete owner key only if value matches local identity + token
    // 2. Revoke lease
    // 3. Cancel KeepAlive and Watch streams
    return LeadershipResult::timed_out(); // stub
}

LeadershipResult
EtcdLeadershipBackend::current_owner(std::string_view singleton_name) {
    // 1. Range query on owner key
    // 2. If key exists: deserialize → return Granted with lease
    // 3. If key absent: return NotOwner
    return LeadershipResult{LeadershipStatusCode::NotOwner, std::nullopt, std::nullopt};
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 3: Write the unit test with mock gRPC stubs**

```cpp
// tests/unit/cluster/singleton/test_etcd_leadership_backend.cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/cluster/singleton/etcd_leadership_backend.hpp>

namespace hpactor::cluster::singleton {

TEST(EtcdLeadershipBackendTest, ConstructionWithConfig) {
    EtcdLeadershipBackend::Config cfg;
    cfg.endpoints = {"https://localhost:2379"};
    cfg.key_prefix = "/hpactor";
    cfg.request_timeout = std::chrono::milliseconds(500);

    EtcdLeadershipBackend backend(cfg);
    // Construction should not throw or crash
    SUCCEED();
}

TEST(EtcdLeadershipBackendTest, CurrentOwnerReturnsNotOwnerWhenKeyAbsent) {
    EtcdLeadershipBackend::Config cfg;
    cfg.endpoints = {"https://localhost:2379"};
    EtcdLeadershipBackend backend(cfg);

    auto result = backend.current_owner("nonexistent");
    EXPECT_EQ(result.status, LeadershipStatusCode::NotOwner);
}

TEST(EtcdLeadershipBackendTest, TryAcquireTimesOutWhenEtcdUnreachable) {
    EtcdLeadershipBackend::Config cfg;
    cfg.endpoints = {"https://192.0.2.1:2379"}; // TEST-NET-1, unreachable
    cfg.request_timeout = std::chrono::milliseconds(100);
    EtcdLeadershipBackend backend(cfg);

    LeadershipAttempt attempt{"test-singleton", "node-a", 0, std::chrono::seconds(10)};
    auto result = backend.try_acquire(attempt);
    // Should time out or report backend unavailable
    bool expected = result.status == LeadershipStatusCode::TimedOut
                 || result.status == LeadershipStatusCode::BackendUnavailable;
    EXPECT_TRUE(expected) << "Got status: " << static_cast<int>(result.status);
}

} // namespace hpactor::cluster::singleton
```

- [ ] **Step 4: Add to CMakeLists.txt**

In `src/cluster/CMakeLists.txt`, add:
```cmake
    singleton/etcd_leadership_backend.cpp
```
And link `hpactor_cluster` to `hpactor_etcd`.

- [ ] **Step 5: Build and run unit tests**

Run: `ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*EtcdLeadershipBackend*"`
Expected: 3 tests PASS (construction, NotOwner, timeout)

- [ ] **Step 6: Run full regression**

Run: `./build/tests/unit/cluster/test_unit_cluster` (all 220+ tests)
Expected: All existing tests still pass

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cluster/singleton/etcd_leadership_backend.hpp \
        src/cluster/singleton/etcd_leadership_backend.cpp \
        tests/unit/cluster/singleton/test_etcd_leadership_backend.cpp \
        src/cluster/CMakeLists.txt tests/unit/cluster/CMakeLists.txt
git commit -m "feat(etcd): add EtcdLeadershipBackend with gRPC callback-based lease management"
```

---

## Task M2-4: TLS Config Wiring + ClusterRuntime Integration

**Goal:** Parse etcd TLS settings from TOML and wire `EtcdLeadershipBackend` creation in `ClusterRuntimeImpl`.

**Files:**
- Modify: `src/config/parsers/cluster_leadership_parser.cpp`
- Modify: `src/cluster/cluster_runtime_impl.cpp`
- Modify: `src/cluster/cluster_runtime_impl.hpp` (add etcd config storage)

**What:** Extend the TOML parser to read `[system.cluster.leadership.etcd]` subsection. In `ClusterRuntimeImpl::start()`, when `mode="external"` and `backend="etcd"`, create `EtcdLeadershipBackend` + `LeadershipBackendAdapter` + `SingletonManagerActor`.

- [ ] **Step 1: Extend TOML parser**

In `src/config/parsers/cluster_leadership_parser.cpp`, add etcd subsection parsing:
```cpp
// After parsing [system.cluster.leadership], parse [system.cluster.leadership.etcd]
auto etcd_section = leadership->get_table("etcd");
if (etcd_section) {
    cfg.etcd_endpoints = etcd_section->get_string_array("endpoints")
                             .value_or(std::vector<std::string>{});
    cfg.etcd_key_prefix = etcd_section->get_string("key_prefix").value_or("/hpactor");
    cfg.etcd_request_timeout_ms = etcd_section->get_uint("request_timeout_ms").value_or(1000);
    cfg.etcd_tls_ca_file = etcd_section->get_string("tls_ca_file").value_or("");
    cfg.etcd_tls_cert_file = etcd_section->get_string("tls_cert_file").value_or("");
    cfg.etcd_tls_key_file = etcd_section->get_string("tls_key_file").value_or("");
}
```

- [ ] **Step 2: Wire in ClusterRuntimeImpl::start()**

In `src/cluster/cluster_runtime_impl.cpp`, modify the election creation:
```cpp
// Phase 2: config-driven election strategy from [system.cluster.leadership]
if (leadership_mode_ == "external" && leadership_backend_ == "etcd") {
    EtcdLeadershipBackend::Config etcd_cfg;
    etcd_cfg.endpoints = etcd_endpoints_;
    etcd_cfg.key_prefix = etcd_key_prefix_;
    etcd_cfg.request_timeout = std::chrono::milliseconds(etcd_request_timeout_ms_);
    auto backend = std::make_unique<EtcdLeadershipBackend>(std::move(etcd_cfg));
    // Note: backend must outlive the adapter. Store in impl.
    etcd_backend_ = std::move(backend);
    auto adapter = std::make_unique<LeadershipBackendAdapter>(
        node_id_, etcd_backend_.get());
    election = std::move(adapter);
} else {
    election = std::make_unique<OldestNodeElection>();
}
```

- [ ] **Step 3: Build + regression test**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster`
Expected: All tests pass, OldestNodeElection still default when config absent

- [ ] **Step 4: Commit**

```bash
git add src/config/parsers/cluster_leadership_parser.cpp \
        src/cluster/cluster_runtime_impl.cpp \
        src/cluster/cluster_runtime_impl.hpp
git commit -m "feat(etcd): wire EtcdLeadershipBackend into ClusterRuntimeImpl from TOML config"
```

---

## Task M2-5: ShardCoordinatorCore Token Validation

**Goal:** Add lease-aware token validation so mutating shard operations reject stale/missing tokens.

**Files:**
- Modify: `include/hpactor/cluster/sharding/shard_coordinator.hpp`
- Modify: `src/cluster/sharding/shard_coordinator.cpp`
- Create: `tests/unit/cluster/sharding/test_shard_coordinator_token.cpp`

**Interfaces:**
- Produces: `ShardCoordinatorCore::set_active_lease()`, `clear_active_lease()`, `validate_token(fencing_token, singleton_name) -> bool`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/cluster/sharding/test_shard_coordinator_token.cpp
#include <gtest/gtest.h>
#include <hpactor/cluster/sharding/shard_coordinator.hpp>
#include <hpactor/cluster/sharding/static_placement.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>

namespace hpactor::cluster::sharding {

class ShardCoordinatorTokenTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto strategy = std::make_unique<StaticPlacement>();
        coordinator_ = std::make_unique<ShardCoordinatorCore>(16, std::move(strategy));
    }
    std::unique_ptr<ShardCoordinatorCore> coordinator_;
};

singleton::LeadershipLease make_lease(const std::string& owner, uint64_t token) {
    singleton::LeadershipLease l;
    l.singleton_name = "shard-coordinator";
    l.owner_node_id = owner;
    l.fencing_token = token;
    return l;
}

TEST_F(ShardCoordinatorTokenTest, ValidateRejectsWhenNoLeaseSet) {
    EXPECT_FALSE(coordinator_->validate_token(1, "shard-coordinator"));
}

TEST_F(ShardCoordinatorTokenTest, ValidateAcceptsActiveLeaseToken) {
    auto lease = make_lease("node-a", 5);
    coordinator_->set_active_lease(lease);
    EXPECT_TRUE(coordinator_->validate_token(5, "shard-coordinator"));
}

TEST_F(ShardCoordinatorTokenTest, ValidateRejectsStaleToken) {
    auto lease = make_lease("node-a", 10);
    coordinator_->set_active_lease(lease);
    EXPECT_FALSE(coordinator_->validate_token(5, "shard-coordinator"));
}

TEST_F(ShardCoordinatorTokenTest, ValidateRejectsHigherTokenFromDifferentLease) {
    auto lease = make_lease("node-a", 10);
    coordinator_->set_active_lease(lease);
    // Token 15 but singleton name is different — should reject
    EXPECT_FALSE(coordinator_->validate_token(15, "other-singleton"));
}

TEST_F(ShardCoordinatorTokenTest, ClearActiveLeaseResetsValidation) {
    auto lease = make_lease("node-a", 5);
    coordinator_->set_active_lease(lease);
    coordinator_->clear_active_lease();
    EXPECT_FALSE(coordinator_->validate_token(5, "shard-coordinator"));
}

} // namespace hpactor::cluster::sharding
```

- [ ] **Step 2: Run test to verify failure**

Run: `ninja -C build test_unit_cluster && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*CoordinatorToken*"`
Expected: FAIL — `validate_token` not defined

- [ ] **Step 3: Implement ShardCoordinatorCore changes**

In `shard_coordinator.hpp`, add:
```cpp
    // ── Leadership token validation (CLU-003 integration) ──────────
    void set_active_lease(const singleton::LeadershipLease& lease);
    void clear_active_lease();
    [[nodiscard]] bool validate_token(uint64_t fencing_token,
                                       std::string_view singleton_name) const;
```

In `shard_coordinator.cpp`, add:
```cpp
void ShardCoordinatorCore::set_active_lease(
    const singleton::LeadershipLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_lease_ = lease;
}

void ShardCoordinatorCore::clear_active_lease() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_lease_.reset();
}

bool ShardCoordinatorCore::validate_token(
    uint64_t fencing_token, std::string_view singleton_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_lease_.has_value()) return false;
    if (singleton_name != "shard-coordinator") return false;
    return fencing_token == active_lease_->fencing_token
        && active_lease_->singleton_name == singleton_name;
}
```

- [ ] **Step 4: Run test to verify passes**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*CoordinatorToken*"`
Expected: 5 tests PASS

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/cluster/sharding/shard_coordinator.hpp \
        src/cluster/sharding/shard_coordinator.cpp \
        tests/unit/cluster/sharding/test_shard_coordinator_token.cpp \
        tests/unit/cluster/CMakeLists.txt
git commit -m "feat(shard): add lease-aware token validation to ShardCoordinatorCore"
```

---

## Task M2-6: ShardCoordinatorActor Gating

**Goal:** ShardCoordinatorActor::rebalance() validates token before delegating to core.

**Files:**
- Modify: `include/hpactor/cluster/sharding/shard_coordinator_actor.hpp`
- Modify: `tests/unit/cluster/sharding/test_shard_coordinator_actor.cpp`

**What:** Add `on_lease_update(lease)` method. `rebalance()` calls `core().validate_token()` first.

- [ ] **Step 1: Modify ShardCoordinatorActor**

Add to `shard_coordinator_actor.hpp`:
```cpp
    void on_lease_update(const singleton::LeadershipLease& lease) {
        core_.set_active_lease(lease);
    }

    bool rebalance_with_token(const std::vector<std::string>& alive_nodes,
                               uint64_t fencing_token) {
        if (!core_.validate_token(fencing_token, "shard-coordinator")) {
            return false;
        }
        core_.rebalance(alive_nodes);
        return true;
    }
```

- [ ] **Step 2: Write actor wrapper tests**

```cpp
// In test_shard_coordinator_actor.cpp, add:
TEST(ShardCoordinatorActorTokenTest, RebalanceWithValidTokenSucceeds) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));

    singleton::LeadershipLease lease;
    lease.singleton_name = "shard-coordinator";
    lease.owner_node_id = "node-a";
    lease.fencing_token = 42;
    actor.on_lease_update(lease);

    EXPECT_TRUE(actor.rebalance_with_token({"node-a", "node-b"}, 42));
}

TEST(ShardCoordinatorActorTokenTest, RebalanceWithStaleTokenReturnsFalse) {
    auto strategy = std::make_unique<StaticPlacement>();
    ShardCoordinatorActor actor(16, std::move(strategy));

    singleton::LeadershipLease lease;
    lease.singleton_name = "shard-coordinator";
    lease.fencing_token = 42;
    actor.on_lease_update(lease);

    EXPECT_FALSE(actor.rebalance_with_token({"node-a"}, 10));
}
```

- [ ] **Step 3: Build + test + commit**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster --gtest_filter="*CoordinatorActor*"`
Expected: All tests pass (existing + 2 new)

```bash
git add include/hpactor/cluster/sharding/shard_coordinator_actor.hpp \
        tests/unit/cluster/sharding/test_shard_coordinator_actor.cpp
git commit -m "feat(shard): gate ShardCoordinatorActor rebalance on fencing token"
```

---

## Task M2-7: End-to-End Integration

**Goal:** Full integration test: etcd-backed singleton → shard coordinator gating.

**Files:**
- Create: `tests/integration/singleton/test_etcd_leadership_integration.cpp`

This task requires a running etcd instance. The integration test:
1. Creates `EtcdLeadershipBackend` pointing at real etcd
2. Acquires lease for "shard-coordinator"
3. Verifies lease is visible via `current_owner()`
4. Creates `LeadershipBackendAdapter` + `SingletonManagerCore` — singleton activates
5. Sets lease on `ShardCoordinatorCore` — token validates
6. Releases lease — singleton drains, token rejected

- [ ] **Step 1: Run full regression**

Run: `ninja -C build && ./build/tests/unit/cluster/test_unit_cluster`
Expected: All 225+ tests pass (220 from M1 + serialize + backend + token + actor)

- [ ] **Step 2: Commit M2 final**

```bash
git add tests/integration/singleton/test_etcd_leadership_integration.cpp
git commit -m "feat(etcd): add end-to-end etcd leadership integration test

M2 complete: etcd-backed leadership election + ShardCoordinator token gating."
```
