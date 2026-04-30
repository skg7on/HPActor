# Cross-Process Actor Examples Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement three framework prerequisite fixes (P1–P3) and two cross-process communication examples (09, 10).

**Architecture:** Three small framework fixes unblock cross-process loopback communication, add actor count query, and fix endpoint byte-order. Two new single-binary examples (each with `--server`/`--client` modes) demonstrate cross-process echo and remote spawn with RPC-style queries. All changes are to existing framework files plus new example files.

**Tech Stack:** C++20, HPActor framework, TCP transport, protobuf WireFrame, CMake/Ninja

**Spec:** `docs/superpowers/specs/2026-04-30-cross-process-examples-design.md`

---

### Task 1: Framework Fix P3 — `parse_endpoint` byte-order

**Files:**
- Modify: `src/net/endpoint.cpp:86`

- [ ] **Step 1: Remove double `htonl()` conversion**

`inet_pton` already returns network byte order in `addr.s_addr`. The extra `htonl()` double-converts on little-endian hosts. Replace line 86:

```cpp
// Before:
return Ipv4Endpoint{htonl(addr.s_addr), htons(static_cast<uint16_t>(port))};
// After:
return Ipv4Endpoint{addr.s_addr, htons(static_cast<uint16_t>(port))};
```

- [ ] **Step 2: Build and run endpoint tests**

```bash
cmake -S . -B build -GNinja && ninja -C build tests/test_communication_endpoint
./build/tests/test_communication_endpoint
```
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add src/net/endpoint.cpp
git commit -m "fix(net): remove double htonl in parse_endpoint

inet_pton already stores network byte order in s_addr. The extra
htonl() double-converted on little-endian hosts, breaking is_loopback()
for addresses parsed via parse_endpoint()."
```

---

### Task 2: Framework Fix P1 — `resolve()` proxy routing on loopback

**Files:**
- Modify: `src/actor/actor_context.cpp:49`

- [ ] **Step 1: Replace `is_local()` guard with endpoint equality check**

In `ActorContext::resolve()`, replace the `is_local()` check at line 49:

```cpp
// Before:
if (!target.is_local()) {
// After:
if (!(target.endpoint == system->endpoint())) {
```

This ensures `ActorProxy` is created for any endpoint that doesn't match the owning system, regardless of whether it's a loopback address. The local registry check (step 2 in `resolve()`) already handles same-process actors.

- [ ] **Step 2: Build and run actor context test**

```bash
ninja -C build tests/test_actor_context
./build/tests/test_actor_context
```
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add src/actor/actor_context.cpp
git commit -m "fix(actor): use endpoint equality instead of is_local() for proxy routing

resolve() used is_local() (loopback IP check) to gate ActorProxy
creation. Two processes on the same host both use loopback, so
cross-process replies were silently dropped. Compare target.endpoint
against system->endpoint() instead."
```

---

### Task 3: Framework Fix P2 — `ActorSystem::actor_count()` API

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` (add public method declaration)
- Modify: `src/actor/actor_system.cpp` (add implementation)

- [ ] **Step 1: Add public method declaration in the header**

In `include/hpactor/core/actor_system.hpp`, after the `get_actor()` declaration (line ~168), add:

```cpp
    // Get the number of live actors in this system
    size_t actor_count() const;
```

- [ ] **Step 2: Add implementation in the .cpp file**

In `src/actor/actor_system.cpp`, after `get_mailbox()` (line ~185), add:

```cpp
size_t ActorSystem::actor_count() const {
    std::lock_guard<std::mutex> lock(actors_mutex_);
    return actors_.size();
}
```

Note: `actors_mutex_` must be made `mutable` since `actor_count()` is `const`. Alternatively, drop the `const` from the declaration.

- [ ] **Step 3: Fix `actors_mutex_` mutability**

Since `actor_count()` is `const` but needs to lock the mutex, either:
- (a) Mark `actors_mutex_` as `mutable` in the header (line ~230):
  ```cpp
  mutable std::mutex actors_mutex_;
  ```
- (b) Drop `const` from `actor_count()`:
  ```cpp
  size_t actor_count();
  ```

Option (a) is cleaner since the method is logically const (observing state).

- [ ] **Step 4: Build**

```bash
ninja -C build
```
Expected: Compiles cleanly

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(actor): add ActorSystem::actor_count() public API

Returns the number of live actors in the system under actors_mutex_.
Used by cross-process examples to query remote system state."
```

---

### Task 4: Example 09 — Cross-Process Echo

**Files:**
- Create: `examples/09_cross_process_echo.cpp`
- Modify: `examples/CMakeLists.txt`

- [ ] **Step 1: Create the example source file**

Write `examples/09_cross_process_echo.cpp`. Architecture:

**Shared infrastructure** (same as spec section "Single Binary, Dual Mode"):
- `--server [port]` / `--client <port>` CLI parsing
- SIGINT handler with `std::atomic<bool> shutdown_requested`
- `make_string_msg(TypeTag, string)` / `extract_string(bytes)` helpers
- `EchoMsgTag` (TypeTag 100)

**Server (`run_server(port)`)**:
- Create `Config` with `enable_network=true`, `endpoint=127.0.0.1:<port>`, `tcp_port=<port>`
- Create `ActorSystem`, spawn `EchoActor` (reuse from Example 01 — inline the class)
- Print `"SERVER: pid=<getpid()> endpoint=127.0.0.1:<port>"`
- Sleep loop until SIGINT
- On shutdown, print `"SERVER: shutting down"`, exit

**Client (`run_client(port)`)**:
- Create `Config` with `enable_network=true`, `endpoint=127.0.0.1:0`, `tcp_port=0`
- Add static route: `config.registrar.static_routes.push_back(net::StaticRouteConfig{Ipv4Endpoint{}, "127.0.0.1", port})`
- Create `ActorSystem`
- Spawn a `ClientActor` that:
  - Constructs the remote EchoActor address: `ActorAddress{server_ep, ActorType{0}, ActorId{1}, 0}` (deterministic since EchoActor is the first spawned user actor)
  - Has a `std::atomic<int> replies_received{0}` and a `std::promise<void> done` or similar signal
  - Sends 3 messages via `context()->send()` in its constructor or `on_activate()`
  - Receives replies in `make_behavior()` callback, prints each, counts
  - After 3 replies, signals completion
- `main()` waits for the client actor to finish, then exits

**ClientActor design** (inline in the example file):

```cpp
class ClientActor : public hpactor::EventBasedActor {
public:
    ClientActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                hpactor::ActorAddress target, int expected_replies,
                std::promise<void> done)
        : hpactor::EventBasedActor(ctx, sys), target_(target),
          expected_(expected_replies), done_(std::move(done)) {
        become(make_behavior());
    }

    void on_activate() override {
        // Send messages once the actor is active
        context()->send(target_, make_string_msg(EchoMsgTag, "hello"));
        context()->send(target_, make_string_msg(EchoMsgTag, "world"));
        context()->send(target_, make_string_msg(EchoMsgTag, "cross-process"));
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == EchoMsgTag) {
                std::cout << "  client received: \""
                          << extract_string(msg.payload()) << "\"" << std::endl;
                if (++received_ >= expected_) {
                    done_.set_value();
                }
            }
        }};
    }

private:
    hpactor::ActorAddress target_;
    int expected_;
    int received_ = 0;
    std::promise<void> done_;
};
```

**Expected output**:
```
=== HPActor Example 09: Cross-Process Echo ===
SERVER: pid=12345 endpoint=127.0.0.1:7000
  EchoActor [1]: received "hello"
  EchoActor [1]: received "world"
  EchoActor [1]: received "cross-process"
SERVER: shutting down
```
Client side:
```
=== HPActor Example 09: Cross-Process Echo ===
  client received: "echo: hello"
  client received: "echo: world"
  client received: "echo: cross-process"
```

- [ ] **Step 2: Add to CMakeLists.txt**

Add to `examples/CMakeLists.txt` after the entry for `08_coroutine_scheduler_demo`:

```cmake
add_executable(09_cross_process_echo 09_cross_process_echo.cpp)
target_link_libraries(09_cross_process_echo PRIVATE hpactor_lib)
```

- [ ] **Step 3: Build**

```bash
ninja -C build examples/09_cross_process_echo
```
Expected: Compiles cleanly

- [ ] **Step 4: Manual integration test**

```bash
# Terminal 1:
./build/examples/09_cross_process_echo --server 7000

# Terminal 2:
./build/examples/09_cross_process_echo --client 7000
```
Expected: Client prints 3 replies, both processes exit cleanly.

- [ ] **Step 5: Commit**

```bash
git add examples/09_cross_process_echo.cpp examples/CMakeLists.txt
git commit -m "feat(examples): add cross-process echo example (09)

Demonstrates enable_network, static routes, context()->send() to
remote ActorAddress, and context()->reply() across process boundaries.
Server spawns EchoActor; client spawns ClientActor that sends 3
messages and prints replies."
```

---

### Task 5: Example 10 — Remote PID Query

**Files:**
- Create: `examples/10_remote_pid_query.cpp`
- Modify: `examples/CMakeLists.txt`

- [ ] **Step 1: Create the example source file**

Write `examples/10_remote_pid_query.cpp`. Architecture:

**Message types and serialization** (in the example file):
```cpp
// Type tags
static const hpactor::TypeTag QueryPidTag{200};
static const hpactor::TypeTag PidResponseTag{201};
static const hpactor::TypeTag QueryActorCountTag{202};
static const hpactor::TypeTag ActorCountResponseTag{203};
static const hpactor::TypeTag ShutdownMsgTag{204};

// Serialization helpers
bytes serialize_pid_response(int pid, const std::string& hostname) {
    bytes b;
    b.push_back((pid >> 24) & 0xFF);
    b.push_back((pid >> 16) & 0xFF);
    b.push_back((pid >> 8) & 0xFF);
    b.push_back(pid & 0xFF);
    b.insert(b.end(), hostname.begin(), hostname.end());
    return b;
}
// ... etc for deserialization, actor count
```

**ProcessInfoActor** — server-side actor spawned remotely by the client:
```cpp
class ProcessInfoActor : public hpactor::EventBasedActor {
public:
    ProcessInfoActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            switch (msg.type_id().value()) {
            case 200: { // QueryPidTag
                char hostname[256]{};
                gethostname(hostname, sizeof(hostname));
                auto payload = serialize_pid_response(getpid(), hostname);
                context()->reply(hpactor::TypedMessage(PidResponseTag, std::move(payload)));
                break;
            }
            case 202: { // QueryActorCountTag
                int count = static_cast<int>(system().actor_count());
                auto payload = serialize_actor_count_response(count);
                context()->reply(hpactor::TypedMessage(ActorCountResponseTag, std::move(payload)));
                break;
            }
            case 204: // ShutdownMsgTag
                set_exit_reason(0);
                break;
            }
        }};
    }
};
```

**Server (`run_server(port)`)**:
- Create `Config` with `enable_network=true`, `endpoint=127.0.0.1:<port>`, `tcp_port=<port>`
- Create `ActorSystem`
- Register type: `system.actor_type_registry().register_type<ProcessInfoActor>("process_info")`
- Print `"SERVER: pid=<getpid()> endpoint=127.0.0.1:<port>"`
- Sleep loop until SIGINT

**Client (`run_client(port)`)**:
- Create `Config` with `enable_network=true`, `endpoint=127.0.0.1:0`, `tcp_port=0`, static route to server
- Create `ActorSystem`
- Call `spawn_remote()`:
  ```cpp
  auto result = system.spawn_remote("127.0.0.1:<port>", "process_info", bytes{});
  if (!result.has_value()) {
      std::cerr << "spawn_remote failed: " << result.error().message() << std::endl;
      return;
  }
  ActorRef remote_ref = result.value();
  ```
- Spawn a local `QueryActor` that takes the `ActorRef` and sends the query sequence:
  - Send `QueryPidTag` → wait for reply → print PID and hostname
  - Send `QueryActorCountTag` → wait for reply → print actor count
  - Send `ShutdownMsgTag` → remote actor terminates
  - Signal done

**QueryActor** (local client actor):
```cpp
class QueryActor : public hpactor::EventBasedActor {
public:
    QueryActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
               hpactor::ActorRef remote, std::promise<void> done)
        : hpactor::EventBasedActor(ctx, sys), remote_(std::move(remote)),
          done_(std::move(done)) {
        become(make_behavior());
    }

    void on_activate() override {
        // Start: send PID query
        step_ = Step::WaitPid;
        context()->send(remote_.address(),
                        hpactor::TypedMessage(QueryPidTag, bytes{}));
    }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            switch (step_) {
            case Step::WaitPid:
                if (msg.type_id() == PidResponseTag) {
                    auto [pid, hostname] = deserialize_pid_response(msg.payload());
                    std::cout << "  Remote PID: " << pid << std::endl;
                    std::cout << "  Remote hostname: " << hostname << std::endl;
                    step_ = Step::WaitActorCount;
                    context()->send(remote_.address(),
                                    hpactor::TypedMessage(QueryActorCountTag, bytes{}));
                }
                break;
            case Step::WaitActorCount:
                if (msg.type_id() == ActorCountResponseTag) {
                    int count = deserialize_actor_count_response(msg.payload());
                    std::cout << "  Remote actor count: " << count << std::endl;
                    step_ = Step::Done;
                    context()->send(remote_.address(),
                                    hpactor::TypedMessage(ShutdownMsgTag, bytes{}));
                    done_.set_value();
                }
                break;
            default: break;
            }
        }};
    }

private:
    enum class Step { WaitPid, WaitActorCount, Done };
    hpactor::ActorRef remote_;
    Step step_ = Step::WaitPid;
    std::promise<void> done_;
};
```

**Expected output** (client side):
```
=== HPActor Example 10: Remote PID Query ===
  Remote PID: 12345
  Remote hostname: myhost.local
  Remote actor count: 2
```

- [ ] **Step 2: Add to CMakeLists.txt**

Add to `examples/CMakeLists.txt` after the Example 09 entry:

```cmake
add_executable(10_remote_pid_query 10_remote_pid_query.cpp)
target_link_libraries(10_remote_pid_query PRIVATE hpactor_lib)
```

- [ ] **Step 3: Build**

```bash
ninja -C build examples/10_remote_pid_query
```
Expected: Compiles cleanly

- [ ] **Step 4: Manual integration test**

```bash
# Terminal 1:
./build/examples/10_remote_pid_query --server 7001

# Terminal 2:
./build/examples/10_remote_pid_query --client 7001
```
Expected: Client prints remote PID, hostname, actor count, both exit cleanly.

- [ ] **Step 5: Commit**

```bash
git add examples/10_remote_pid_query.cpp examples/CMakeLists.txt
git commit -m "feat(examples): add remote PID query example (10)

Demonstrates spawn_remote(), ActorTypeRegistry::register_type(),
RPC-style request/response with custom struct serialization, and
ActorRef as location-transparent handle to remotely spawned actors."
```

---

### Task 6: Test Script and Final Verification

**Files:**
- Create: `examples/test_cross_process.sh`

- [ ] **Step 1: Create the test script**

```bash
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"

echo "=== Test 09: Cross-Process Echo ==="
"$BUILD_DIR/examples/09_cross_process_echo" --server 7000 &
S09=$!
sleep 1
"$BUILD_DIR/examples/09_cross_process_echo" --client 7000
kill $S09 2>/dev/null; wait $S09 2>/dev/null

echo ""
echo "=== Test 10: Remote PID Query ==="
"$BUILD_DIR/examples/10_remote_pid_query" --server 7001 &
S10=$!
sleep 1
"$BUILD_DIR/examples/10_remote_pid_query" --client 7001
kill $S10 2>/dev/null; wait $S10 2>/dev/null

echo ""
echo "=== All cross-process tests passed ==="
```

- [ ] **Step 2: Run the test script**

```bash
chmod +x examples/test_cross_process.sh
./examples/test_cross_process.sh
```
Expected: Both examples pass, exit code 0

- [ ] **Step 3: Commit**

```bash
git add examples/test_cross_process.sh
git commit -m "test(examples): add cross-process integration test script"
```

---

### Task 7: Run Full Test Suite (Verification)

- [ ] **Step 1: Build all targets**

```bash
ninja -C build tests/all
```

- [ ] **Step 2: Run full test suite**

```bash
cd build && ctest --output-on-failure -j$(sysctl -n hw.ncpu)
```
Expected: All 65 tests pass (no regressions from P1–P3 fixes)

- [ ] **Step 3: Verify no untracked files**

```bash
git status
```
Expected: Only committed files, no stray changes
