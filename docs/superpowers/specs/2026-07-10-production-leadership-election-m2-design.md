# M2: etcd Leadership Backend + Shard Coordinator Gating — Design Spec

## 1. Executive Summary

M2 delivers the first real external coordinator production path. The etcd backend implements `ILeadershipBackend` using gRPC callback API (exception-free). An isolated protobuf serialization TU follows the existing `toml_parser.cpp` pattern for `-fexceptions`. ShardCoordinatorCore gains token validation so mutations are gated on current leadership.

## 2. etcd Client Architecture Decision

**etcd-cpp-apiv3 rejected.** The library is fundamentally exception-based and its `BUILD_WITH_NO_EXCEPTIONS` flag is broken (issues #278, #259). No path to `-fno-exceptions` compatibility.

**Direct gRPC integration chosen.** The etcd v3 API surface needed for leadership election is small: 4 RPCs across 3 services (Lease, KV, Watch). gRPC C++ callback API (1.40+) is completely exception-free — all errors via `grpc::Status`.

### 2.1 Exception Isolation Strategy

Follow the existing pattern: `toml_parser.cpp` is compiled with `-fexceptions` as an isolated TU. Similarly:

```
src/etcd/                              # Compiled with -fexceptions
  proto/                               # Vendored etcd protos
  etcd_serialize.h / etcd_serialize.cpp  # Protobuf ↔ LeadershipLease conversion
  etcd_stub_factory.h / .cpp             # gRPC channel + stub creation (uses protobuf)
  etcd_lease_client.h / .cpp             # LeaseGrant, LeaseKeepAlive (bidirectional stream)
  etcd_kv_client.h / .cpp               # Txn, Range
  etcd_watch_client.h / .cpp             # Watch (bidirectional stream), compaction recovery

include/hpactor/cluster/singleton/     # -fno-exceptions (normal)
  etcd_leadership_backend.hpp           # Public header — no protobuf types
```

**Boundary contract:** `src/etcd/` files may use exceptions internally. All callbacks crossing into actor code return error codes via `LeadershipResult`. No exception crosses the boundary.

### 2.2 gRPC Callback API Mapping

```cpp
// LeaseGrant — unary RPC, callback-based
void lease_grant(int64_t ttl_seconds,
    std::function<void(std::optional<int64_t> lease_id, grpc::Status)> on_done);

// Txn — unary RPC, callback-based
void txn_compare_and_swap(
    const std::string& key, const std::string& value, int64_t lease_id,
    std::function<void(bool success, int64_t revision, std::string current_value, grpc::Status)> on_done);

// Watch — bidirectional streaming, callback per event
// Returns a WatchHandle that can be cancelled
WatchHandle watch_key(
    const std::string& key, int64_t start_revision,
    std::function<void(WatchEvent event)> on_event);  // WatchEvent = Put{key,value,revision} | Delete{key,revision} | Compacted{compact_revision}

// LeaseKeepAlive — bidirectional streaming
void lease_keepalive_start(int64_t lease_id,
    std::function<void(bool alive)> on_keepalive_response);
void lease_keepalive_stop();
```

These are plain-C++-types callbacks — no protobuf types reach actor code.

## 3. Task Breakdown

### Task M2-1: Vendor etcd Protos + CMake Integration

**Files:**
- Create: `src/etcd/proto/rpc.proto` (from etcd v3.5.x)
- Create: `src/etcd/proto/kv.proto` (from etcd mvccpb)
- Create: `src/etcd/CMakeLists.txt` — protoc + gRPC codegen, compile with `-fexceptions`
- Modify: `src/CMakeLists.txt` — `add_subdirectory(etcd)`

**What:** Strip gogoproto/google.api annotations from vendored protos. Generate C++ gRPC stubs. Compile generated code with `-fexceptions` in isolated TU.

**Verification:** `ninja -C build hpactor_etcd` compiles clean. Generated stubs are linkable.

### Task M2-2: etcd Serialize — LeadershipLease ↔ protobuf

**Files:**
- Create: `src/etcd/etcd_serialize.hpp` (exceptions allowed in .cpp)
- Create: `src/etcd/etcd_serialize.cpp`

**Interface (exception-free header boundary):**
```cpp
namespace hpactor::etcd {

// Serialize LeadershipLease to etcd key value (JSON or protobuf bytes)
std::string serialize_lease(const cluster::singleton::LeadershipLease& lease);

// Deserialize from etcd key value
std::optional<cluster::singleton::LeadershipLease> deserialize_lease(std::string_view data);

// Serialize cluster identity for lease metadata
std::string serialize_identity(const std::string& node_id, uint64_t incarnation);

} // namespace hpactor::etcd
```

**Decision:** Use JSON encoding for the etcd key value (human-readable, debuggable with `etcdctl`). Protobuf encoding adds complexity without benefit for a single-field key.

**Verification:** Unit tests: round-trip serialize/deserialize, empty data → nullopt, corrupt data → nullopt.

### Task M2-3: EtcdLeadershipBackend

**Files:**
- Create: `include/hpactor/cluster/singleton/etcd_leadership_backend.hpp`
- Create: `src/cluster/singleton/etcd_leadership_backend.cpp`
- Create: `tests/unit/cluster/singleton/test_etcd_leadership_backend.cpp` (unit tests with mock gRPC)
- Create: `tests/integration/singleton/test_etcd_leadership_integration.cpp` (integration tests with real etcd)

**Class:**
```cpp
class EtcdLeadershipBackend : public ILeadershipBackend {
  public:
    struct Config {
        std::vector<std::string> endpoints;  // ["https://etcd-1:2379", ...]
        std::string key_prefix = "/hpactor";
        std::chrono::milliseconds request_timeout{1000};
        // TLS (optional)
        std::string tls_ca_file;
        std::string tls_cert_file;
        std::string tls_key_file;
    };

    explicit EtcdLeadershipBackend(Config cfg);

    LeadershipResult try_acquire(const LeadershipAttempt& attempt) override;
    LeadershipResult renew(const LeadershipLease& lease) override;
    LeadershipResult release(const LeadershipLease& lease) override;
    LeadershipResult current_owner(std::string_view singleton_name) override;
};
```

**Implementation details:**

- **try_acquire:** Create etcd lease with TTL → Txn: if key not exists, put owner value with lease → on success, extract `mod_revision` as fencing_token → start `LeaseKeepAlive` stream + `Watch` on owner key
- **renew:** `LeaseKeepAlive` keeps lease alive → Txn verify owner value still matches local identity + token → return renewed lease with updated revision as fencing_token
- **release:** Txn: delete key only if value matches local identity → revoke lease → cancel Watch and KeepAlive
- **current_owner:** Range query on owner key → deserialize → return current owner

**Thread model:** gRPC completion queue runs on dedicated thread(s). Callbacks post results to an `std::promise`/`std::future` pair. The backend's public methods block on the future with a timeout (bounded blocking). This keeps blocking work off actor scheduler hot paths.

**Unit tests (mock gRPC):**
- acquire: lease created, txn succeeds, token from mod_revision
- acquire: key already owned by other → returns AlreadyOwned
- acquire: etcd unreachable → BackendUnavailable
- renew: same owner → returns Renewed with higher token
- renew: different owner → returns Lost
- release: key matches → Released, lease revoked
- current_owner: key exists → returns owner
- current_owner: key absent → NotOwner

**Integration tests (real etcd):**
- acquire → renew → release cycle against real etcd
- Two backends, one acquires → other sees AlreadyOwned
- Watch fires on key delete
- Lease expiry auto-deletes key

### Task M2-4: TLS Config Wiring

**Files:**
- Modify: `src/config/parsers/cluster_leadership_parser.cpp` — add etcd TLS fields
- Modify: `src/cluster/cluster_runtime_impl.cpp` — create `EtcdLeadershipBackend` when mode=external, backend=etcd

**What:** Parse `[system.cluster.leadership.etcd]` section: endpoints, key_prefix, request_timeout_ms, tls_ca_file, tls_cert_file, tls_key_file. Wire into `ClusterRuntimeImpl::start()`.

**Verification:** Parser unit test — all etcd fields parsed with defaults. Cluster runtime integration test — `EtcdLeadershipBackend` created with config from TOML.

### Task M2-5: ShardCoordinatorCore Token Validation

**Files:**
- Modify: `include/hpactor/cluster/sharding/shard_coordinator.hpp`
- Modify: `src/cluster/sharding/shard_coordinator.cpp`
- Create: `tests/unit/cluster/sharding/test_shard_coordinator_token.cpp`

**What:** Add to `ShardCoordinatorCore`:
```cpp
// Set the active lease for this coordinator instance
void set_active_lease(const singleton::LeadershipLease& lease);

// Clear the active lease (on step-down)
void clear_active_lease();

// Validate a fencing token before a mutating operation
bool validate_token(uint64_t fencing_token, std::string_view singleton_name) const;
```

**Validate rules:** Reject if:
- No active lease set (NotOwner)
- `singleton_name != "shard-coordinator"` (wrong singleton)
- `fencing_token < active_lease.fencing_token` (stale)
- `fencing_token == active_lease.fencing_token && owner_node_id != active_lease.owner_node_id` (different owner, same token — shouldn't happen with backend-issued tokens)

**Verification:** Unit tests — accept valid token, reject stale/lower token, reject with no lease, reject wrong singleton name.

### Task M2-6: ShardCoordinatorActor Integration

**Files:**
- Modify: `include/hpactor/cluster/sharding/shard_coordinator_actor.hpp`
- Modify: `src/cluster/sharding/shard_coordinator_actor.cpp`

**What:** `ShardCoordinatorActor::rebalance()` checks `validate_token()` before delegating to core. The lease is set from `SingletonManagerActor` via a new method:
```cpp
void on_lease_update(const singleton::LeadershipLease& lease);
```

**Verification:** Actor wrapper tests — rebalance with valid token succeeds, rebalance with stale token returns FencingTokenStale.

### Task M2-7: End-to-End Integration

**Files:**
- Create: `tests/integration/cluster/test_leadership_shard_integration.cpp`

**What:** Full integration test: `ActorSystem::enable_cluster()` with etcd-backed leadership → singleton activates with backend lease → shard coordinator receives lease → rebalance accepted with valid token → failover to new node → old node's token rejected.

**Verification:** Uses embedded etcd or testcontainer. Marks M2 complete.

## 4. Build Impact

New dependencies for M2:
- gRPC (C++ callback API, ≥1.40)
- Protobuf (already a dependency)
- Abseil (transitive via gRPC)
- OpenSSL (TLS for etcd connections, already a dependency)

CMake changes:
- `src/etcd/CMakeLists.txt` — protoc codegen, exception-enabled TU
- Protobuf compiled with `-Dprotobuf_DISABLE_RTTI=ON` for `-fno-rtti` compatibility
- gRPC and etcd stubs linked into `hpactor_cluster` (or a separate `hpactor_etcd` static lib)

## 5. Consul Backend (Deferred)

Consul backend is out of scope for M2. It is listed as M2-optional in the plan and will be added only if deployment demand requires it. The `ILeadershipBackend` interface was designed for this — Consul support is a new implementation, no interface changes needed.

## 6. Open Decisions for Implementation

1. **gRPC completion queue model:** Single shared CQ with tag-based dispatch vs per-stub CQ. Start with per-stub for simplicity.
2. **LeaseKeepAlive frequency:** etcd recommends every TTL/3. Set to `renew_interval_ms` from config.
3. **Watch recovery after compaction:** Compacted watch → linearizable Range → new Watch from returned revision. Max 3 retries before reporting BackendUnavailable.
4. **TLS for tests:** Use embedded etcd without TLS in tests. Production TLS verified via integration test.
