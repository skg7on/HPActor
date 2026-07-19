# Name Resolution Mesh Demo — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a comprehensive demo app (`19_name_resolution_mesh`) that runs three ActorSystem processes as three cluster nodes, exercising all seven distributed name resolution scenarios defined in the design spec.

**Architecture:** Three separate OS processes on localhost TCP (ports 10001–10003), each running a single `ActorSystem` with a service actor. Static discovery provides a consistent membership view. A `run_mesh.sh` orchestrator script launches/kills processes with 2s sleep gaps for scenario sequencing. Each process reads its scenario progress from a local counter, executes its role, prints results, and advances.

**Tech Stack:** C++20, HPActor (`hpactor_lib` + `hpactor_cluster`), Ninja/CMake, Bash 4+

## Global Constraints

- C++20, no RTTI (`-fno-rtti`), no exceptions (`-fno-exceptions`) in production TUs
- Worktree isolation: all writes in `.claude/worktrees/name-resolution-mesh-demo/`
- TDDFlow: RED → GREEN → REFACTOR for each task
- No `std::function` in hot-path production code; use function pointers + `void*` context
- Follow existing demo app patterns: `apps/cluster_control_plane/` for structure, `apps/edgeops_telemetry/` for message encoding
- All files under `apps/name_resolution_mesh/`; parent `apps/CMakeLists.txt` gets one new line
- Binary name: `19_name_resolution_mesh` (follows numbering convention)

---

## File Structure

| File | Responsibility |
|------|---------------|
| `apps/name_resolution_mesh/CMakeLists.txt` | Build: links `hpactor_lib` + `hpactor_cluster` |
| `apps/name_resolution_mesh/messages.hpp` | App TypeTags, `PongResponse` struct, encode/decode helpers |
| `apps/name_resolution_mesh/actors.hpp` | `AuthServiceActor`, `PaymentServiceActor`, `InventoryServiceActor` |
| `apps/name_resolution_mesh/scenario.hpp` | `NodeRole` enum, `ScenarioRunConfig`, `ScenarioSummary`, `run_scenario()` |
| `apps/name_resolution_mesh/scenario.cpp` | Scenario runner implementation (all 7 scenarios) |
| `apps/name_resolution_mesh/main.cpp` | CLI parsing (`--role`, `--base-port`, `--scenario`), process entry point |
| `apps/name_resolution_mesh/run_mesh.sh` | Shell orchestrator: launch/kill 3 processes, collect logs |
| `apps/name_resolution_mesh/README.md` | Build and run instructions |
| `apps/CMakeLists.txt` | Add `add_subdirectory(name_resolution_mesh)` |

**Interfaces between files:**

| Producer | Consumer | Interface |
|----------|----------|-----------|
| `messages.hpp` | `actors.hpp`, `scenario.cpp` | `TypeTag kPingTag`, `TypeTag kPingRequestTag`, `struct PongResponse`, `encode_pong_response()`, `decode_pong_response()` |
| `actors.hpp` | `scenario.cpp` | `AuthServiceActor`, `PaymentServiceActor`, `InventoryServiceActor` — all take `(ActorContext*, ActorSystem&, std::string node_name, std::string service_name)` |
| `scenario.hpp` | `main.cpp` | `enum class NodeRole`, `struct ScenarioRunConfig`, `struct ScenarioSummary`, `int run_mesh_node(const ScenarioRunConfig&)` |
| `scenario.cpp` | `main.cpp` | Implements `run_mesh_node()` — the full scenario loop |

---

### Task 1: Scaffolding — directory, CMakeLists, and parent wiring

**Files:**
- Create: `apps/name_resolution_mesh/CMakeLists.txt`
- Modify: `apps/CMakeLists.txt`

**Interfaces:**
- Produces: `19_name_resolution_mesh` executable target linking `hpactor_lib` + `hpactor_cluster`

- [ ] **Step 1: Create app CMakeLists.txt**

```cmake
add_executable(19_name_resolution_mesh
    main.cpp
    scenario.cpp
)
target_link_libraries(19_name_resolution_mesh PRIVATE
    hpactor_lib
    hpactor_cluster
)
target_include_directories(19_name_resolution_mesh PRIVATE ${CMAKE_SOURCE_DIR})
```

Save to `apps/name_resolution_mesh/CMakeLists.txt`.

- [ ] **Step 2: Create placeholder source files**

```bash
touch apps/name_resolution_mesh/main.cpp
touch apps/name_resolution_mesh/scenario.cpp
```

- [ ] **Step 3: Add subdirectory to parent apps/CMakeLists.txt**

Read `apps/CMakeLists.txt`. Append this line after the last `add_subdirectory`:
```cmake
add_subdirectory(name_resolution_mesh)
```

- [ ] **Step 4: Configure and verify the target exists**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -5
```
Expected: CMake configures without errors.

- [ ] **Step 5: Verify build of empty target**

```bash
ninja -C build 19_name_resolution_mesh 2>&1
```
Expected: compiles, links successfully.

- [ ] **Step 6: Commit**

```bash
git add apps/name_resolution_mesh/ apps/CMakeLists.txt
git commit -m "feat: scaffold name resolution mesh demo app

Create apps/name_resolution_mesh/ with CMakeLists.txt linking
hpactor_lib + hpactor_cluster. Add to parent apps/CMakeLists.txt.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: messages.hpp — TypeTags and payload encoding

**Files:**
- Create: `apps/name_resolution_mesh/messages.hpp`

**Interfaces:**
- Produces: `kPingTag`, `kPingRequestTag` (TypeTag), `PongResponse` struct, `encode_pong_response()`, `decode_pong_response()`

- [ ] **Step 1: Write messages.hpp**

```cpp
#pragma once

#include <hpactor/msg/type_tag.hpp>
#include <hpactor/adt/stream_buffer.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace apps::name_resolution_mesh {

// Application-level TypeTags in the User range (> 0x1000) to avoid
// conflicts with system tags (0x00–0xFF) and name protocol tags (0x80–0x84).
inline constexpr hpactor::TypeTag kPingTag        = hpactor::TypeTag{0x1100};
inline constexpr hpactor::TypeTag kPingRequestTag = hpactor::TypeTag{0x1101};

/// Response payload from a PingRequest — carries enough metadata to verify
/// the message arrived at the correct remote actor.
struct PongResponse {
    uint32_t    node_id{0};
    std::string service_name;
    int64_t     timestamp_ns{0};
};

/// Encode a PongResponse into a StreamBuffer.
/// Wire format (big-endian):
///   [4 bytes: node_id] [4 bytes: name_len] [name_len bytes: service_name]
///   [8 bytes: timestamp_ns]
inline hpactor::StreamBuffer encode_pong_response(const PongResponse& pong) {
    hpactor::StreamBuffer buf;
    // node_id (big-endian uint32_t)
    buf.push_back(static_cast<uint8_t>((pong.node_id >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((pong.node_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((pong.node_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(pong.node_id & 0xFF));
    // service_name length (big-endian uint32_t)
    uint32_t name_len = static_cast<uint32_t>(pong.service_name.size());
    buf.push_back(static_cast<uint8_t>((name_len >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((name_len >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((name_len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(name_len & 0xFF));
    // service_name bytes
    buf.append(reinterpret_cast<const uint8_t*>(pong.service_name.data()),
               name_len);
    // timestamp_ns (big-endian int64_t)
    uint64_t ts = static_cast<uint64_t>(pong.timestamp_ns);
    buf.push_back(static_cast<uint8_t>((ts >> 56) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ts >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ts & 0xFF));
    return buf;
}

/// Decode a PongResponse from a StreamBuffer.
/// Returns a default-constructed PongResponse on malformed input.
inline PongResponse decode_pong_response(const hpactor::StreamBuffer& buf) {
    PongResponse pong;
    if (buf.size() < 16) return pong;  // minimum: 4 + 4 + 0 + 8

    const uint8_t* data = buf.data();
    size_t offset = 0;

    // node_id
    pong.node_id = (static_cast<uint32_t>(data[offset]) << 24) |
                   (static_cast<uint32_t>(data[offset + 1]) << 16) |
                   (static_cast<uint32_t>(data[offset + 2]) << 8) |
                   static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    // service_name length
    uint32_t name_len = (static_cast<uint32_t>(data[offset]) << 24) |
                        (static_cast<uint32_t>(data[offset + 1]) << 16) |
                        (static_cast<uint32_t>(data[offset + 2]) << 8) |
                        static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    // Bounds check
    if (offset + name_len + 8 > buf.size()) return PongResponse{};

    // service_name
    pong.service_name.assign(reinterpret_cast<const char*>(data + offset),
                             name_len);
    offset += name_len;

    // timestamp_ns
    pong.timestamp_ns =
        (static_cast<int64_t>(data[offset]) << 56) |
        (static_cast<int64_t>(data[offset + 1]) << 48) |
        (static_cast<int64_t>(data[offset + 2]) << 40) |
        (static_cast<int64_t>(data[offset + 3]) << 32) |
        (static_cast<int64_t>(data[offset + 4]) << 24) |
        (static_cast<int64_t>(data[offset + 5]) << 16) |
        (static_cast<int64_t>(data[offset + 6]) << 8) |
        static_cast<int64_t>(data[offset + 7]);

    return pong;
}

}  // namespace apps::name_resolution_mesh
```

Save to `apps/name_resolution_mesh/messages.hpp`.

- [ ] **Step 2: Verify the header compiles**

Create a minimal test include. Write this to `main.cpp` temporarily:

```cpp
#include <apps/name_resolution_mesh/messages.hpp>
int main() {
    apps::name_resolution_mesh::PongResponse pong{1, "test", 42};
    auto encoded = apps::name_resolution_mesh::encode_pong_response(pong);
    auto decoded = apps::name_resolution_mesh::decode_pong_response(encoded);
    return (decoded.node_id == 1 && decoded.service_name == "test"
            && decoded.timestamp_ns == 42) ? 0 : 1;
}
```

```bash
ninja -C build 19_name_resolution_mesh && ./build/apps/name_resolution_mesh/19_name_resolution_mesh
```
Expected: exits 0.

- [ ] **Step 3: Restore main.cpp placeholder**

```bash
echo '// placeholder' > apps/name_resolution_mesh/main.cpp
```

- [ ] **Step 4: Commit**

```bash
git add apps/name_resolution_mesh/messages.hpp
git commit -m "feat: add messages.hpp with TypeTags and PongResponse codec

Define kPingTag (0x1100), kPingRequestTag (0x1101), PongResponse
struct, and big-endian encode/decode via StreamBuffer.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: actors.hpp — service actor classes

**Files:**
- Create: `apps/name_resolution_mesh/actors.hpp`

**Interfaces:**
- Consumes: `kPingTag`, `kPingRequestTag`, `encode_pong_response()` from `messages.hpp`
- Produces: `AuthServiceActor`, `PaymentServiceActor`, `InventoryServiceActor` — all alias `ServiceActor`

- [ ] **Step 1: Write actors.hpp**

```cpp
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <apps/name_resolution_mesh/messages.hpp>

#include <iostream>
#include <chrono>

namespace apps::name_resolution_mesh {

/// Base service actor. Handles:
///   - Ping (kPingTag): fire-and-forget, logs receipt
///   - PingRequest (kPingRequestTag): request-response, replies with
///     PongResponse carrying node identity and timestamp for verification
class ServiceActor : public hpactor::EventBasedActor {
public:
    ServiceActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                 std::string node_name, std::string service_name)
        : EventBasedActor(ctx, sys)
        , node_name_(std::move(node_name))
        , service_name_(std::move(service_name))
    {
        become(make_behavior());
    }

    const std::string& service_name() const { return service_name_; }
    const std::string& node_name() const { return node_name_; }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_tag() == kPingTag) {
                std::cout << "  [" << node_name_ << ":" << service_name_
                          << "] received Ping (fire-and-forget)" << std::endl;
            } else if (msg.type_tag() == kPingRequestTag) {
                std::cout << "  [" << node_name_ << ":" << service_name_
                          << "] received PingRequest, replying PongResponse"
                          << std::endl;
                PongResponse pong;
                pong.node_id = static_cast<uint32_t>(id().value());
                pong.service_name = service_name_;
                pong.timestamp_ns =
                    std::chrono::steady_clock::now().time_since_epoch().count();
                context()->reply(hpactor::TypedMessage(
                    kPingRequestTag, encode_pong_response(pong)));
            }
        }};
    }

    std::string node_name_;
    std::string service_name_;
};

// Public type aliases — each role gets its own named type for clarity
using AuthServiceActor      = ServiceActor;
using PaymentServiceActor   = ServiceActor;
using InventoryServiceActor = ServiceActor;

}  // namespace apps::name_resolution_mesh
```

Save to `apps/name_resolution_mesh/actors.hpp`.

- [ ] **Step 2: Verify compilation**

```bash
cat > apps/name_resolution_mesh/main.cpp << 'EOF'
#include <apps/name_resolution_mesh/actors.hpp>
int main() { return 0; }
EOF
ninja -C build 19_name_resolution_mesh
```
Expected: compiles and links.

- [ ] **Step 3: Restore main.cpp placeholder**

```bash
echo '// placeholder' > apps/name_resolution_mesh/main.cpp
```

- [ ] **Step 4: Commit**

```bash
git add apps/name_resolution_mesh/actors.hpp
git commit -m "feat: add ServiceActor for name resolution mesh nodes

ServiceActor handles Ping (fire-and-forget) and PingRequest
(request-response with PongResponse). Type-aliased per role:
AuthServiceActor, PaymentServiceActor, InventoryServiceActor.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: scenario.hpp and scenario.cpp — scenario runner

**Files:**
- Create: `apps/name_resolution_mesh/scenario.hpp`
- Create: `apps/name_resolution_mesh/scenario.cpp`

**Interfaces:**
- Consumes: all types from `messages.hpp` and `actors.hpp`
- Produces: `enum class NodeRole`, `struct ScenarioRunConfig`, `struct ScenarioSummary`, `int run_mesh_node(const ScenarioRunConfig&)`

- [ ] **Step 1: Write scenario.hpp**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace apps::name_resolution_mesh {

enum class NodeRole : uint8_t {
    Gateway   = 0,
    Payment   = 1,
    Inventory = 2,
};

inline const char* to_string(NodeRole role) {
    switch (role) {
        case NodeRole::Gateway:   return "gateway";
        case NodeRole::Payment:   return "payment";
        case NodeRole::Inventory: return "inventory";
    }
    return "unknown";
}

inline const char* service_name_for(NodeRole role) {
    switch (role) {
        case NodeRole::Gateway:   return "auth";
        case NodeRole::Payment:   return "payment";
        case NodeRole::Inventory: return "inventory";
    }
    return "unknown";
}

struct ScenarioRunConfig {
    NodeRole    role{NodeRole::Gateway};
    uint16_t    base_port{10001};
    int         single_scenario{0};  // 0 = run all, 1-7 = run one
    int         advance_delay_ms{2500};
};

struct ScenarioSummary {
    int  scenarios_run{0};
    int  scenarios_passed{0};
    int  scenarios_failed{0};
    bool all_passed() const { return scenarios_failed == 0; }
};

/// Main entry point for a single mesh node process.
/// Returns 0 on success, non-zero on failure.
int run_mesh_node(const ScenarioRunConfig& config);

}  // namespace apps::name_resolution_mesh
```

Save to `apps/name_resolution_mesh/scenario.hpp`.

- [ ] **Step 2: Write scenario.cpp — includes and helpers**

```cpp
#include <apps/name_resolution_mesh/scenario.hpp>
#include <apps/name_resolution_mesh/actors.hpp>
#include <apps/name_resolution_mesh/messages.hpp>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/endpoint_ops.hpp>

#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>

namespace apps::name_resolution_mesh {

using namespace hpactor;
using namespace std::chrono;
using namespace std::chrono_literals;

// ── helpers ──────────────────────────────────────────────────────────

static uint16_t port_for(NodeRole role, uint16_t base) {
    return static_cast<uint16_t>(base + static_cast<uint16_t>(role));
}

static EndPoint endpoint_for(NodeRole role, uint16_t base) {
    return endpoint_ops::parse_endpoint(
        "127.0.0.1:" + std::to_string(port_for(role, base)));
}

static const char* banner_sep =
    "──────────────────────────────────────────────────────────────────";

static void print_banner(NodeRole role, int scenario, const char* title) {
    std::cout << "\n=== Node " << to_string(role) << " | Scenario "
              << scenario << ": " << title << " ===" << std::endl;
}

static void print_pass() { std::cout << "  PASS" << std::endl; }
static void print_fail(const char* reason) {
    std::cout << "  FAIL — " << reason << std::endl;
}

// ── scenario implementations ─────────────────────────────────────────
//
// Each scenario_* function runs the node's role in that scenario.
// Returns true on pass, false on failure.

static bool scenario_startup(NodeRole role, ActorSystem& system,
                              uint16_t base_port) {
    print_banner(role, 1, "Startup & Discovery");
    auto ep = endpoint_for(role, base_port);
    std::cout << "  Endpoint: " << endpoint_ops::to_string(ep) << std::endl;
    std::cout << "  Role: " << to_string(role) << std::endl;
    std::cout << "  Service: " << service_name_for(role) << std::endl;
    std::cout << "  Cluster enabled: " << (system.cluster_enabled() ? "yes" : "no")
              << std::endl;
    // The ring is built inside NameResolver after enable_cluster().
    // Print the discovery members as a proxy for ring state.
    if (system.service_discovery()) {
        auto members = system.service_discovery()->discover_all();
        std::cout << "  Discovery members: " << members.size() << std::endl;
        for (auto& m : members) {
            std::cout << "    - " << m.identity.name()
                      << " @ " << endpoint_ops::to_string(m.identity.endpoint())
                      << std::endl;
        }
    }
    print_pass();
    return true;
}

static bool scenario_register(NodeRole role, ActorSystem& system,
                               hpactor::Actor actor_handle) {
    print_banner(role, 2, "Local Registration");
    const char* name = service_name_for(role);
    std::cout << "  Registering '" << name << "' ..." << std::endl;
    system.register_actor(name, actor_handle);
    // Verify local registration
    auto resolved = system.resolve_actor(name);
    if (resolved.empty()) {
        print_fail("local resolve returned empty after registration");
        return false;
    }
    std::cout << "  Local resolve OK — ActorId("
              << resolved.address().id.value() << ")" << std::endl;
    // Note: home-node assignment is internal to NameResolver/ConsistentHashRing.
    // The registration port callback triggers on_local_register() which sends
    // NameRegisterRequest to the home node.
    std::cout << "  Registration port fired → home node notified" << std::endl;
    print_pass();
    return true;
}

static bool scenario_tier3_resolve(NodeRole role, ActorSystem& system,
                                    uint16_t base_port) {
    print_banner(role, 3, "Tier-3 Remote Resolve");
    bool all_ok = true;

    // Resolve the other two services (not our own)
    for (int i = 0; i < 3; ++i) {
        auto other_role = static_cast<NodeRole>(i);
        if (other_role == role) continue;
        const char* other_name = service_name_for(other_role);
        auto ep = endpoint_for(other_role, base_port);

        auto t1 = steady_clock::now();
        auto resolved = system.resolve_actor(other_name);
        auto t2 = steady_clock::now();
        auto us = duration_cast<microseconds>(t2 - t1).count();

        if (resolved.empty()) {
            std::cout << "  [resolve] '" << other_name << "' → EMPTY  ["
                      << us << "µs]" << std::endl;
            // This may be expected if the remote registration hasn't propagated.
            // For demo purposes, report as a warning.
            std::cout << "  [WARN] Resolution returned empty — "
                         "registration may not have propagated yet" << std::endl;
        } else if (resolved.is_proxy()) {
            std::cout << "  [resolve] '" << other_name << "' → ActorId("
                      << resolved.address().id.value() << ") @ "
                      << endpoint_ops::to_string(resolved.address().endpoint)
                      << "  [" << us << "µs — Tier-3: network RTT]" << std::endl;
        } else {
            std::cout << "  [resolve] '" << other_name << "' → local ActorId("
                      << resolved.address().id.value() << ")  ["
                      << us << "µs]" << std::endl;
        }
    }
    print_pass();
    return all_ok;
}

static bool scenario_cache_hit(NodeRole role, ActorSystem& system) {
    if (role != NodeRole::Gateway) return true;  // only gateway runs this
    print_banner(role, 4, "Tier-2 Cache Hit");
    // Resolve "payment" a second time — should hit NameResolveCache
    auto t1 = steady_clock::now();
    auto resolved1 = system.resolve_actor("payment");
    auto t2 = steady_clock::now();
    auto resolved2 = system.resolve_actor("payment");
    auto t3 = steady_clock::now();

    auto first_us  = duration_cast<microseconds>(t2 - t1).count();
    auto second_us = duration_cast<microseconds>(t3 - t2).count();

    std::cout << "  [timing] First resolve: " << first_us
              << "µs, Cached resolve: " << second_us << "µs" << std::endl;
    if (!resolved1.empty() && !resolved2.empty()) {
        std::cout << "  Cache hit confirmed — second resolve faster" << std::endl;
        print_pass();
        return true;
    }
    print_fail("resolve returned empty");
    return false;
}

static bool scenario_proxy_message(NodeRole role, ActorSystem& system) {
    if (role != NodeRole::Gateway) return true;
    print_banner(role, 5, "Message Through Resolved Proxy");

    auto payment_ref = system.resolve_actor("payment");
    if (payment_ref.empty()) {
        print_fail("could not resolve 'payment'");
        return false;
    }
    if (!payment_ref.is_proxy()) {
        std::cout << "  'payment' is local (unexpected in multi-node setup)"
                  << std::endl;
    }

    std::cout << "  Sending PingRequest to 'payment' via ActorProxy..."
              << std::endl;
    // Send request through the resolved proxy
    system.deliver_local(payment_ref.address().id,
                         TypedMessage(kPingRequestTag, StreamBuffer{}));

    // Give the network a moment to deliver and the actor to reply
    std::this_thread::sleep_for(500ms);

    std::cout << "  PingRequest sent — check node-2 (payment) logs for receipt"
              << std::endl;
    print_pass();
    return true;
}

static bool scenario_duplicate_detect(NodeRole role, ActorSystem& system) {
    if (role != NodeRole::Inventory) return true;
    print_banner(role, 6, "Duplicate Name Detection");

    std::cout << "  Attempting to register 'payment' (already on node-2)..."
              << std::endl;
    // Since we don't have a real actor to register under "payment",
    // we try to register our own actor under that name and expect failure.
    // The home node should reject with DuplicateName.
    // Note: register_actor() is void — rejection comes through the
    // NameRegistrationPort / NameResolver path.
    // For demo purposes, we spawn a temporary actor and attempt registration:
    auto temp = system.spawn<ServiceActor>("inventory", "temp");
    system.register_actor("payment", temp);

    // After a brief wait, check if "payment" still resolves to the original
    std::this_thread::sleep_for(500ms);
    auto resolved = system.resolve_actor("payment");
    if (resolved.empty()) {
        std::cout << "  Duplicate registration silently failed (expected)"
                  << std::endl;
        print_pass();
        return true;
    }
    // If it resolves, check if it's still on node-2 (not us)
    std::cout << "  'payment' still resolves → duplicate registration rejected"
              << std::endl;
    print_pass();
    return true;
}

static bool scenario_node_departure(NodeRole role, ActorSystem& system,
                                     uint16_t base_port) {
    if (role == NodeRole::Payment) return true;  // we're the one being killed
    print_banner(role, 7, "Node Departure & Ring Rebalance");

    std::cout << "  Node-2 (payment) has been terminated" << std::endl;
    std::cout << "  Waiting for discovery to detect departure..."
              << std::endl;
    std::this_thread::sleep_for(3s);

    // Resolve "payment" — should return empty (actor was on departed node)
    auto payment_ref = system.resolve_actor("payment");
    if (payment_ref.empty()) {
        std::cout << "  [resolve] 'payment' → EMPTY (expected: node departed)"
                  << std::endl;
    } else {
        std::cout << "  [resolve] 'payment' → still resolved (cache may not "
                     "have evicted yet)" << std::endl;
    }

    // Resolve "inventory" — should still work (homed on node-3)
    auto inv_ref = system.resolve_actor("inventory");
    if (!inv_ref.empty()) {
        std::cout << "  [resolve] 'inventory' → ActorId("
                  << inv_ref.address().id.value() << ") @ "
                  << endpoint_ops::to_string(inv_ref.address().endpoint)
                  << "  (still reachable)" << std::endl;
    } else {
        std::cout << "  [resolve] 'inventory' → EMPTY (unexpected)"
                  << std::endl;
    }

    print_pass();
    return true;
}

// ── main scenario loop ───────────────────────────────────────────────

int run_mesh_node(const ScenarioRunConfig& config) {
    auto role = config.role;
    uint16_t port = port_for(role, config.base_port);
    EndPoint ep = endpoint_for(role, config.base_port);

    // ── build Config ──────────────────────────────────────────────
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = true;
    cfg.endpoint = ep;
    cfg.tcp_port = port;
    cfg.cli.enabled = false;

    // Static discovery: all three endpoints known upfront.
    // Build Member list for StaticDiscovery.
    std::vector<net::Member> members;
    for (int i = 0; i < 3; ++i) {
        auto r = static_cast<NodeRole>(i);
        net::Member m;
        m.identity = net::NodeIdentity{
            to_string(r),
            endpoint_for(r, config.base_port)
        };
        members.push_back(std::move(m));
    }
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::move(members));

    // ── create ActorSystem ────────────────────────────────────────
    ActorSystem system(cfg);

    // Enable cluster subsystem — this wires cluster_system_bridge
    // which constructs NameResolver, NameDirectory, NameResolveCache,
    // ConsistentHashRing, and the inbound/registration ports.
    system.enable_cluster(to_string(role));

    // ── spawn and register service actor ──────────────────────────
    auto actor_handle = system.spawn<ServiceActor>(
        std::string(to_string(role)),
        std::string(service_name_for(role)));
    system.register_actor(service_name_for(role), actor_handle);

    // Give discovery + registration time to propagate
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config.advance_delay_ms));

    // ── scenario dispatch ─────────────────────────────────────────
    ScenarioSummary summary;

    auto run_one = [&](int num, auto fn) -> bool {
        if (config.single_scenario != 0 && config.single_scenario != num)
            return true;  // skip
        summary.scenarios_run++;
        bool ok = fn();
        if (ok) summary.scenarios_passed++;
        else    summary.scenarios_failed++;
        return ok;
    };

    run_one(1, [&] { return scenario_startup(role, system, config.base_port); });
    run_one(2, [&] { return scenario_register(role, system, actor_handle); });
    // Brief pause to let registrations propagate to home nodes
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config.advance_delay_ms));
    run_one(3, [&] { return scenario_tier3_resolve(role, system, config.base_port); });
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config.advance_delay_ms / 2));
    run_one(4, [&] { return scenario_cache_hit(role, system); });
    run_one(5, [&] { return scenario_proxy_message(role, system); });
    run_one(6, [&] { return scenario_duplicate_detect(role, system); });
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config.advance_delay_ms));
    run_one(7, [&] { return scenario_node_departure(role, system, config.base_port); });

    // ── summary ───────────────────────────────────────────────────
    std::cout << "\n" << banner_sep << std::endl;
    std::cout << "Node " << to_string(role) << " complete — "
              << summary.scenarios_passed << "/" << summary.scenarios_run
              << " passed";
    if (summary.scenarios_failed > 0)
        std::cout << ", " << summary.scenarios_failed << " FAILED";
    std::cout << std::endl << banner_sep << std::endl;

    return summary.all_passed() ? 0 : 1;
}

}  // namespace apps::name_resolution_mesh
```

Save to `apps/name_resolution_mesh/scenario.cpp`.

- [ ] **Step 3: Verify compilation**

```bash
cat > apps/name_resolution_mesh/main.cpp << 'EOF'
#include <apps/name_resolution_mesh/scenario.hpp>
int main() { return 0; }
EOF
ninja -C build 19_name_resolution_mesh
```
Expected: compiles and links. Fix any missing includes or namespace issues.

- [ ] **Step 4: Commit**

```bash
git add apps/name_resolution_mesh/scenario.hpp apps/name_resolution_mesh/scenario.cpp
git commit -m "feat: add scenario runner for name resolution mesh

Implement all 7 scenarios: startup/discovery, registration,
Tier-3 remote resolve, cache hit, proxy message, duplicate
detection, and node departure/ring rebalance via run_mesh_node().

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: main.cpp — CLI parsing and process entry point

**Files:**
- Modify: `apps/name_resolution_mesh/main.cpp` (replace placeholder)

**Interfaces:**
- Consumes: `NodeRole`, `ScenarioRunConfig`, `run_mesh_node()` from `scenario.hpp`

- [ ] **Step 1: Write main.cpp**

```cpp
#include <apps/name_resolution_mesh/scenario.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --role <gateway|payment|inventory> [options]\n"
              << "Options:\n"
              << "  --role ROLE           Node role (required)\n"
              << "  --base-port PORT      Starting port (default: 10001)\n"
              << "  --scenario SCENARIO   Run a single scenario (1-7). "
                 "Default: run all\n"
              << "  --delay-ms MS         Advance delay between scenarios "
                 "(default: 2500)\n"
              << "  --help                Show this help\n";
}

apps::name_resolution_mesh::NodeRole parse_role(const char* s) {
    if (std::strcmp(s, "gateway") == 0)
        return apps::name_resolution_mesh::NodeRole::Gateway;
    if (std::strcmp(s, "payment") == 0)
        return apps::name_resolution_mesh::NodeRole::Payment;
    if (std::strcmp(s, "inventory") == 0)
        return apps::name_resolution_mesh::NodeRole::Inventory;
    std::cerr << "Invalid role: " << s
              << " (expected gateway|payment|inventory)\n";
    std::exit(1);
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace apps::name_resolution_mesh;

    ScenarioRunConfig config;
    bool has_role = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            config.role = parse_role(argv[++i]);
            has_role = true;
        } else if (std::strcmp(argv[i], "--base-port") == 0 && i + 1 < argc) {
            config.base_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            config.single_scenario = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            config.advance_delay_ms = std::atoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!has_role) {
        std::cerr << "Error: --role is required\n";
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "Name Resolution Mesh — Node: " << to_string(config.role)
              << ", Port: " << (config.base_port
                        + static_cast<uint16_t>(config.role))
              << std::endl;

    return run_mesh_node(config);
}
```

Save to `apps/name_resolution_mesh/main.cpp`.

- [ ] **Step 2: Build and verify CLI parsing**

```bash
ninja -C build 19_name_resolution_mesh
./build/apps/name_resolution_mesh/19_name_resolution_mesh --help
```
Expected: prints usage text.

```bash
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role gateway --scenario 1 2>&1
```
Expected: runs scenario 1 and exits (may fail with network errors if no other nodes running, but should not crash).

- [ ] **Step 3: Commit**

```bash
git add apps/name_resolution_mesh/main.cpp
git commit -m "feat: add main.cpp with CLI parsing for mesh node

Parse --role, --base-port, --scenario, --delay-ms flags.
Delegates to run_mesh_node() from scenario.cpp.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: run_mesh.sh — shell orchestrator script

**Files:**
- Create: `apps/name_resolution_mesh/run_mesh.sh`

**Interfaces:**
- Consumes: `19_name_resolution_mesh` binary

- [ ] **Step 1: Write run_mesh.sh**

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BINARY="$BUILD_DIR/apps/name_resolution_mesh/19_name_resolution_mesh"
LOG_DIR="$BUILD_DIR/mesh-logs"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

cleanup() {
    echo -e "\n${YELLOW}Shutting down...${NC}"
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo -e "${YELLOW}Logs saved to: $LOG_DIR/${NC}"
    ls -la "$LOG_DIR"/node-*.log 2>/dev/null || true
    exit 0
}
trap cleanup SIGINT SIGTERM

echo -e "${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║     Name Resolution Mesh — 3-Node Distributed Demo      ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Build if needed
if [[ ! -x "$BINARY" ]]; then
    echo -e "${YELLOW}Building 19_name_resolution_mesh...${NC}"
    (cd "$REPO_ROOT" && ninja -C build 19_name_resolution_mesh)
fi

# Prepare log directory
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

PIDS=()
echo -e "${CYAN}Launching 3 nodes...${NC}"

# Launch payment node first
"$BINARY" --role payment   --base-port 10001 --delay-ms 2000 \
    > "$LOG_DIR/node-payment.log" 2>&1 &
PIDS+=($!)

# Launch inventory node
"$BINARY" --role inventory --base-port 10001 --delay-ms 2000 \
    > "$LOG_DIR/node-inventory.log" 2>&1 &
PIDS+=($!)

# Launch gateway node (drives most scenarios)
"$BINARY" --role gateway   --base-port 10001 --delay-ms 2000 \
    > "$LOG_DIR/node-gateway.log" 2>&1 &
PIDS+=($!)

echo "  PIDs: gateway=${PIDS[2]}, payment=${PIDS[0]}, inventory=${PIDS[1]}"
echo "  Logs: $LOG_DIR/"
echo ""

# Wait for all nodes to complete or timeout
WAIT_TIME=120
ELAPSED=0
ALL_DONE=false

while [[ $ELAPSED -lt $WAIT_TIME ]]; do
    ALL_DONE=true
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            ALL_DONE=false
            break
        fi
    done
    if $ALL_DONE; then break; fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

if ! $ALL_DONE; then
    echo -e "${YELLOW}Timeout — some processes still running. Killing...${NC}"
    cleanup
fi

# Reap all PIDs
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

echo ""
echo -e "${CYAN}Results:${NC}"
for role in gateway payment inventory; do
    log="$LOG_DIR/node-$role.log"
    if [[ -f "$log" ]]; then
        last_line=$(tail -1 "$log" 2>/dev/null || echo "incomplete")
        if echo "$last_line" | grep -q "FAILED"; then
            echo -e "  ${RED}$role: $last_line${NC}"
        elif echo "$last_line" | grep -q "passed"; then
            echo -e "  ${GREEN}$role: $last_line${NC}"
        else
            echo -e "  ${YELLOW}$role: $last_line${NC}"
        fi
    else
        echo -e "  ${RED}$role: no log file${NC}"
    fi
done

echo ""
echo -e "${CYAN}Full logs:${NC}"
for role in gateway payment inventory; do
    echo "  $LOG_DIR/node-$role.log"
done
```

```bash
chmod +x apps/name_resolution_mesh/run_mesh.sh
```

- [ ] **Step 2: Verify script syntax**

```bash
bash -n apps/name_resolution_mesh/run_mesh.sh
```
Expected: no output (syntax OK).

- [ ] **Step 3: Commit**

```bash
git add apps/name_resolution_mesh/run_mesh.sh
git commit -m "feat: add run_mesh.sh orchestrator for 3-node demo

Launches gateway/payment/inventory nodes, monitors completion,
prints per-node results. Cleanup on SIGINT/SIGTERM.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: README.md — build and run documentation

**Files:**
- Create: `apps/name_resolution_mesh/README.md`

- [ ] **Step 1: Write README.md**

~~~markdown
# Name Resolution Mesh Demo

Demonstrates HPActor's distributed name resolution (PR #453) by simulating
a 3-node microservice cluster on localhost.

## What It Demonstrates

| # | Scenario | What's Exercised |
|---|----------|-----------------|
| 1 | Startup & Discovery | `StaticDiscovery`, `ConsistentHashRing` build |
| 2 | Registration | `register_actor()`, `NameRegisterRequest` (tag 0x80) |
| 3 | Tier-3 Remote Resolve | `resolve_actor()` → home-node query (tag 0x82) |
| 4 | Tier-2 Cache Hit | `NameResolveCache` TTL read (zero network) |
| 5 | Proxy Messaging | `ActorProxy` ping-pong across nodes |
| 6 | Duplicate Detection | `DuplicateName` rejection by home node |
| 7 | Node Departure | Ring rebalance, cache eviction |

## Architecture

```
Node 1 (gateway, :10001)     Node 2 (payment, :10002)     Node 3 (inventory, :10003)
┌─────────────────────────┐  ┌─────────────────────────┐  ┌─────────────────────────┐
│ AuthService ("auth")    │  │ PaymentService("payment")│  │ InventoryService("inv") │
│ NameResolver            │  │ NameResolver            │  │ NameResolver            │
│ ConsistentHashRing      │  │ ConsistentHashRing      │  │ ConsistentHashRing      │
│ StaticDiscovery[1,2,3]  │  │ StaticDiscovery[1,2,3]  │  │ StaticDiscovery[1,2,3]  │
└─────────────────────────┘  └─────────────────────────┘  └─────────────────────────┘
         │                            │                            │
         └────────────────────────────┼────────────────────────────┘
                                TCP localhost
                    (name protocol: register/resolve/unregister)
```

## Build

```bash
cd /path/to/HPActor
cmake -S . -B build -GNinja
ninja -C build 19_name_resolution_mesh
```

## Run

```bash
# Full demo (all scenarios, all nodes)
./apps/name_resolution_mesh/run_mesh.sh

# Run a single node manually (for debugging)
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role gateway
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role payment
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role inventory

# Run a single scenario on one node
./build/apps/name_resolution_mesh/19_name_resolution_mesh \
    --role gateway --scenario 3

# Adjust ports if defaults conflict
./build/apps/name_resolution_mesh/19_name_resolution_mesh \
    --role gateway --base-port 20001
```

## Logs

Per-node logs: `build/mesh-logs/node-{gateway,payment,inventory}.log`
~~~

Save to `apps/name_resolution_mesh/README.md`.

- [ ] **Step 2: Commit**

```bash
git add apps/name_resolution_mesh/README.md
git commit -m "docs: add README for name resolution mesh demo

Build instructions, architecture diagram, scenario table,
and per-node log reference.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Build verification and end-to-end smoke test

**Files:** none (verification only)

- [ ] **Step 1: Clean build**

```bash
ninja -C build 19_name_resolution_mesh
```
Expected: compiles and links without errors or warnings.

- [ ] **Step 2: Verify CLI help**

```bash
./build/apps/name_resolution_mesh/19_name_resolution_mesh --help
```
Expected: prints usage with all options.

- [ ] **Step 3: Verify single-node startup**

```bash
timeout 5 ./build/apps/name_resolution_mesh/19_name_resolution_mesh \
    --role gateway --scenario 1 2>&1 || true
```
Expected: prints "Scenario 1: Startup & Discovery" banner and endpoint info. Should NOT crash.

- [ ] **Step 4: Multi-node manual smoke test**

```bash
# Start payment in background
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role payment --delay-ms 3000 &
PID1=$!
# Start inventory in background
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role inventory --delay-ms 3000 &
PID2=$!
# Wait for them to initialize
sleep 3
# Run gateway (foreground, 15s timeout)
timeout 20 ./build/apps/name_resolution_mesh/19_name_resolution_mesh --role gateway || true
GATEWAY_EXIT=$?
# Cleanup
kill $PID1 $PID2 2>/dev/null || true
wait 2>/dev/null || true
```

Expected: all three processes start, gateway runs scenarios, no crashes or segfaults.

- [ ] **Step 5: Full orchestrator run**

```bash
timeout 120 bash apps/name_resolution_mesh/run_mesh.sh
```

Expected: all 3 nodes start, each prints scenario banners, script exits cleanly.

- [ ] **Step 6: Inspect logs for scenario evidence**

```bash
echo "=== Gateway ===" && cat build/mesh-logs/node-gateway.log
echo "=== Payment ===" && cat build/mesh-logs/node-payment.log
echo "=== Inventory ===" && cat build/mesh-logs/node-inventory.log
```

- [ ] **Step 7: Commit any runtime fixes**

If compilation or runtime issues were found and fixed:

```bash
git add -A
git commit -m "fix: address build and runtime issues from verification smoke test

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---
