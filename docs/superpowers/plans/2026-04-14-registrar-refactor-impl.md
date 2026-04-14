# Registrar Refactor Implementation Plan

**Date:** 2026-04-14
**Owner:** SKG7ON
**Status:** Planned
**Based on:** `docs/superpowers/specs/2026-04-14-registrar-refactor-design.md`

---

## Phase 1: Foundation Fixes (registrar.cpp)

### Task 1.1: Fix Null Pointer Issue in start_client_mode()
**File:** `src/net/registrar.cpp`
**Dependencies:** None
**Changes:**
1. Add member `std::unique_ptr<NodeRegistry> client_registry_` to `UdpRegistrar`
2. Add member `int udp_socket_ = -1` to `UdpRegistrar` (for sending responses)
3. Modify `start_client_mode()` to:
   - Create `client_registry_` and populate with static routes
   - Pass `client_registry_.get()` to `RegistrarClient`
4. Modify `start_server_mode()` to create UDP socket for responses
5. Modify `stop()` to clean up `client_registry_` and `udp_socket_`

### Task 1.2: Fix UDP Response in handle_udp_packet()
**File:** `src/net/registrar.cpp`
**Dependencies:** Task 1.1 (udp_socket_ member needed)
**Changes:**
1. In `handle_udp_packet()`, when responding to ResolveQuery:
   - Replace `(void)response; (void)from_host; (void)from_port;` with actual `sendto()`
   - Use stored `udp_socket_` to send response to `from_host:from_port`

---

## Phase 2: RegistrarServer Fixes (registrar_server.cpp)

### Task 2.1: Add Helper Function — send_tcp_message()
**File:** `src/net/registrar_server.cpp`
**Dependencies:** None
**Changes:**
1. Add private method to build and send TCP responses:
```cpp
void send_tcp_response(int client_fd, TcpMessageType type, const bytes& payload);
```

### Task 2.2: Refactor handle_accept() to Await Register First
**File:** `src/net/registrar_server.cpp`
**Dependencies:** Task 2.1
**Changes:**
1. Restructure `handle_accept()`:
   - First, do a blocking read to get the Register message (with timeout)
   - Parse the NodeId from the Register payload
   - Add to `clients_` map before entering the read loop
   - If no valid Register received within timeout, close fd and return
   - Then proceed with existing message loop
2. Extract the message loop into a separate method `read_messages_loop()`

### Task 2.3: Implement handle_tcp_message()
**File:** `src/net/registrar_server.cpp`
**Dependencies:** Task 2.1, Task 2.2
**Changes:**
1. Parse message type from header byte 5
2. Implement `Register` (0x01) handling:
   - Parse full payload including AcceptorInfo
   - Create NodeEndpoint and upsert to registry
   - Send `Accept` response with error code 0
   - Call `broadcast_node_joined()`
3. Implement `Heartbeat` (0x02) handling:
   - Find client by fd, update `last_seen` in registry
4. Add error case for unknown/invalid messages

---

## Phase 3: RegistrarClient Fixes (registrar_client.cpp)

### Task 3.1: Add get_local_ip() Function
**File:** `src/net/registrar_client.cpp`
**Dependencies:** None
**Changes:**
1. Add includes: `<ifaddrs.h>`, `<net/if.h>`
2. Add static/helper function `get_local_ip()`:
   - Use `getifaddrs()` to enumerate interfaces
   - Return first non-loopback, up, running IPv4 address
   - Fallback to "127.0.0.1" on failure

### Task 3.2: Update send_registration() with AcceptorInfo
**File:** `src/net/registrar_client.cpp`
**Dependencies:** None
**Changes:**
1. Add member `std::vector<AcceptorInfo> acceptors_` to `RegistrarClient`
2. Add setter: `void set_acceptors(std::vector<AcceptorInfo> acceptors)`
3. Modify `send_registration()`:
   - Replace hardcoded "127.0.0.1" with `get_local_ip()`
   - Serialize AcceptorCount + AcceptorInfo[] after TcpPort

### Task 3.3: Complete Failover Logic
**File:** `src/net/registrar_client.cpp`
**Dependencies:** Tasks 1.1, 3.1
**Changes:**
1. Add `handle_connection_lost()` method:
   - Called when connection error or heartbeat timeout detected
   - Stop heartbeat thread
   - Try to bind TCP port (election)
   - If bind succeeds: signal UdpRegistrar to become server
   - If bind fails: call `find_server_via_broadcast()`
2. Add `find_server_via_broadcast()` method:
   - Create UDP broadcast socket
   - Send probe to discover servers
   - Wait for response, parse server endpoint
   - Update registry and reconnect
3. Wire up heartbeat timeout detection in `heartbeat_loop()`

---

## Phase 4: Header Updates (registrar.hpp)

### Task 4.1: Update UdpRegistrar Header
**File:** `include/hpactor/net/registrar.hpp`
**Dependencies:** Tasks 1.1, 1.2
**Changes:**
1. Add `std::unique_ptr<NodeRegistry> client_registry_` member
2. Add `int udp_socket_ = -1` member
3. Update `start_client_mode()` signature comments

### Task 4.2: Update RegistrarClient Header
**File:** `include/hpactor/net/registrar.hpp`
**Dependencies:** Tasks 3.2, 3.3
**Changes:**
1. Add `std::vector<AcceptorInfo> acceptors_` member
2. Add `void set_acceptors(std::vector<AcceptorInfo>)` method declaration
3. Add `handle_connection_lost()` and `find_server_via_broadcast()` declarations

---

## Phase 5: Build & Test

### Task 5.1: Build Verification
**Command:**
```bash
cmake -S . -B build -GNinja && ninja -C build
```
**Expected:** Clean build with no errors

### Task 5.2: Run Existing Tests
**Command:**
```bash
ctest --output-on-failure
```
**Expected:** All existing tests pass

### Task 5.3: Verify Spec Completion
- [ ] Issue 1: `handle_tcp_message()` processes Register messages
- [ ] Issue 2: `clients_` map populated on accept
- [ ] Issue 3: RegistrarClient gets valid registry pointer
- [ ] Issue 4: UDP responses actually sent
- [ ] Issue 5: Local IP detected (not hardcoded)
- [ ] Issue 6: AcceptorInfo in registration payload
- [ ] Issue 7: Failover logic complete

---

## Dependency Graph

```
Phase 1 ──┬── Task 1.1 ── Task 1.2
          │                │
          │                └──────┐
          │                         │
Phase 2 ──┼── Task 2.1               │
          │    │                     │
          │    └────── Task 2.2 ─────┤
          │              │           │
          │              └──── Task 2.3
          │
Phase 3 ──┼── Task 3.1 ──┬── Task 3.2
          │              │
          │              └──── Task 3.3
          │
Phase 4 ──┼── Task 4.1
          │
          └── Task 4.2

Phase 5: Independent (runs after all above)
```

---

## File Summary

| File | Tasks |
|------|-------|
| `include/hpactor/net/registrar.hpp` | 4.1, 4.2 |
| `src/net/registrar.cpp` | 1.1, 1.2 |
| `src/net/registrar_server.cpp` | 2.1, 2.2, 2.3 |
| `src/net/registrar_client.cpp` | 3.1, 3.2, 3.3 |
| `build/` | 5.1, 5.2 |
