# ActorSystem Phase 4 Frame and Stream Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Invoke the
> repository `.claude/skills/tddflow-development/` skill before production
> edits and `superpowers:verification-before-completion` before commits or
> completion claims. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace split `ConnectionPool`/`ActorSystem` protocol demultiplexing
with one typed `InboundFrameRouter`, and move bounded peer-qualified stream
registry/handler ownership into `StreamRuntime` without moving network
lifecycle ownership.

**Architecture:** Plain and TLS connections emit one canonical encoded HPAC
frame shape. `ConnectionPool` performs strict typed decode and forwards valid
frames or decode failures through a fixed `InboundFrameSink`.
`InboundFrameRouter` classifies every valid envelope and delegates to concrete
`MessagingRuntime`, `RpcChannel`, and `StreamRuntime` owners. `StreamRuntime`
uses one bounded mutex-protected session registry and never invokes actor,
delivery, transport, log, metric, or callback code under its lock.

**Tech Stack:** C++20, protobuf, CMake, Ninja, GoogleTest 1.14, CTest
architecture checks, HPActor `result<T>`, ASan, TSAN, socket-pair integration
tests, no RTTI, no exception-based control flow.

## Global Constraints

- Start only after Phase 0 through Phase 3 are merged into `origin/main` and
  their focused normal/ASan/TSAN evidence is available.
- Execute in `.claude/worktrees/actor-system-frame-stream-routing/` on branch
  `refactor/actor-system-frame-stream-routing`, created from updated
  `origin/main`. This follows `.claude/rules` category/description and worktree
  naming requirements.
- Before every write, verify `pwd` ends in
  `.claude/worktrees/actor-system-frame-stream-routing` and
  `git branch --show-current` prints
  `refactor/actor-system-frame-stream-routing`.
- Follow RED -> GREEN -> REFACTOR for every production change. Run and record
  the stated RED command before editing its production files.
- Preserve existing public `ActorSystem`, `WireFrame::decode`, stream, and
  direct transport handler signatures, defaults, constness, return values, and
  `noexcept` guarantees.
- Keep transport, connection pool, event-loop/thread, discovery, location
  cache, RPC channel, HTTP, timers, and remote-spawn ownership in the Phase 1
  shell. Do not create `NetworkRuntime` on this branch.
- `InboundFrameRouter` is the only valid-envelope classifier when the unified
  sink is installed. One frame must never reach both unified and legacy
  handlers.
- Normalize connection callbacks to exactly one complete
  `WireFrame::encode()` byte sequence, including the eight-byte HPAC header.
- Enforce `max_inbound_frame_bytes = 16 MiB`,
  `max_batch_entries = 1024`, and `max_active_streams = 4096` before allocating
  or iterating beyond those bounds.
- Preserve protobuf field numbers, TypeTags, HPAC magic/header, actor delivery
  metadata, batch entry order, and current ordinary/RPC/backpressure semantics.
- Use request-only `AckRequested` for ordinary reliable data; dual-bit
  `AckRequested | AckResponse` for legacy ACK response; response-only
  `AckResponse` for legacy NACK. Support dedicated ACK/NACK oneofs without
  switching all outgoing traffic to them.
- Ordinary and batch actor data use full Phase 3 messaging delivery. Stream
  protocol actor delivery uses only
  `FastDeliveryReason::StreamProtocol`.
- Never hold the stream registry mutex across actor spawn/stop, message
  delivery, wire output, logging, metrics, snapshot formatting, or callbacks.
- Preserve mailbox MPSC, reservation/release, ready-gate, single-consumer, and
  lost-wakeup contracts.
- Do not introduce `dynamic_cast`, `typeid`, exception control flow, generic
  service lookup, virtual router interfaces, per-frame handler allocation, a
  component-wide router mutex, or public runtime component headers.
- Do not complete or claim complete `StreamHandle::write`, remote stream output,
  retransmission, or durable streaming.
- Use the worktree's own `build/`, `build-asan/`, and `build-tsan/` directories.

## Design References

- `docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`
- `docs/superpowers/specs/2026-06-28-actor-system-phase3-messaging-runtime-design.md`
- `docs/superpowers/specs/2026-06-28-actor-system-phase4-frame-stream-routing-design.md`
- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- `docs/architecture/production/production-reliability-plane.md`
- `docs/architecture/production/actor-delivery-semantics-design.md`

## Expected File Structure

**Create:**

- `include/hpactor/net/frame_dispatch_result.hpp` — fixed-size decode/dispatch
  result enums and counters.
- `include/hpactor/net/inbound_frame_sink.hpp` — peer context and fixed
  function-pointer sink.
- `include/hpactor/actor/stream_snapshot.hpp` — bounded read-only registry
  snapshot values.
- `src/net/inbound_frame_router.hpp` — private concrete router API.
- `src/net/inbound_frame_router.cpp` — envelope classification and delegation.
- `src/actor/stream_runtime.hpp` — private stream owner API/data model.
- `src/actor/stream_runtime.cpp` — bounded registry and stream frame handlers.
- `tests/unit/net/test_frame_decode_result.cpp`
- `tests/unit/net/test_inbound_frame_router.cpp`
- `tests/unit/actor/test_stream_runtime.cpp`
- `tests/integration/net/test_inbound_frame_framing.cpp`
- `tests/integration/actor/test_inbound_frame_routing.cpp`
- `tests/integration/actor/test_stream_runtime_concurrency.cpp`
- `tests/architecture/assert_frame_stream_routing_boundaries.cmake`

**Modify:**

- `include/hpactor/msg/frame.hpp`
- `src/msg/frame.cpp`
- `include/hpactor/net/connection.hpp`
- `include/hpactor/net/wireframe_connection.hpp`
- `src/net/wireframe_connection.cpp`
- `include/hpactor/net/tls_connection.hpp`
- `src/net/tls_connection.cpp`
- `include/hpactor/net/connection_pool.hpp`
- `src/net/connection_pool.cpp`
- `include/hpactor/net/tcp_transport.hpp`
- `src/net/tcp_transport.cpp`
- `src/net/CMakeLists.txt`
- `src/actor/CMakeLists.txt`
- `src/runtime/actor_system_impl.hpp`
- `src/runtime/actor_system_impl.cpp`
- `include/hpactor/actor/actor_system.hpp`
- `src/actor/actor_system.cpp`
- `src/cli/commands/stream_commands.cpp`
- `include/hpactor/metrics/metrics_event.hpp` only for the reviewed bounded
  result codes.
- `tests/unit/net/CMakeLists.txt`
- `tests/unit/actor/CMakeLists.txt`
- `tests/integration/net/CMakeLists.txt`
- `tests/integration/actor/CMakeLists.txt`
- `tests/integration/actor/test_batch_messaging.cpp`
- `tests/integration/actor/test_remote_delivery_result.cpp`
- `tests/integration/actor/test_remote_backpressure_signals.cpp`
- `tests/integration/mailbox/test_reliable_messaging.cpp`
- `tests/integration/rpc/test_rpc_channel.cpp`
- `tests/architecture/CMakeLists.txt`
- `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`
- `CLAUDE_MEMORY.md`

Private component headers remain under `src/`. Focused unit targets may add
`${CMAKE_SOURCE_DIR}/src` as a private include directory.

---

### Task 0: Create the worktree, verify prerequisites, and inventory ingress

**Deliverable:** A clean Phase 4 worktree, passing focused baseline, and a
committed frame/stream ownership and lifetime inventory.

**Interfaces:**

- Consumes: merged Phase 3 `MessagingRuntime`, typed reliable/backpressure
  methods, stable network ports, and `FastDeliveryReason::StreamProtocol`.
- Produces: reviewed inventory used to verify every later owner/callback move.

- [ ] **Step 1: Create the required worktree**

Run from the main checkout:

```bash
git fetch origin
git worktree add -b refactor/actor-system-frame-stream-routing \
  .claude/worktrees/actor-system-frame-stream-routing origin/main
cd .claude/worktrees/actor-system-frame-stream-routing
pwd
git branch --show-current
git status --short
```

Expected: correct path/branch and empty status.

- [ ] **Step 2: Verify Phase 3 contracts exist**

```bash
test -f src/runtime/messaging_runtime.hpp
test -f docs/superpowers/specs/2026-06-28-actor-system-phase4-frame-stream-routing-design.md
rg -n "class MessagingRuntime|FastDeliveryReason::StreamProtocol" src
rg -n "on_reliable_ack|on_reliable_nack|on_remote_backpressure" src/runtime
```

Expected: all files/symbols exist. If not, stop rather than creating a parallel
pre-Phase-3 router.

- [ ] **Step 3: Read mandatory guidance**

```bash
sed -n '1,260p' AGENTS.md
sed -n '1,320p' CLAUDE.md
sed -n '1,260p' CLAUDE_MEMORY.md
sed -n '1,320p' .claude/rules
sed -n '1,360p' \
  docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md
sed -n '1,280p' \
  docs/architecture/production/production-reliability-plane.md
```

Expected: newer rules are recorded and take precedence.

- [ ] **Step 4: Configure and build the focused baseline**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build hpactor_lib test_unit_net test_unit_actor test_unit_msg \
  test_integration_net test_integration_actor test_integration_mailbox \
  test_integration_rpc
```

Expected: all targets build.

- [ ] **Step 5: Run the focused baseline**

```bash
./build/tests/unit/net/test_unit_net \
  --gtest_filter='*WireFrame*:*Connection*:*Transport*'
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='*Stream*'
./build/tests/unit/msg/test_unit_msg \
  --gtest_filter='*Ack*:*Nack*:*OutboundDeliveryTracker*'
./build/tests/integration/net/test_integration_net \
  --gtest_filter='*WireFrame*:*Connection*:*Tls*:*Transport*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*Batch*:*RemoteDelivery*:*Backpressure*:*Stream*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='*Reliable*'
./build/tests/integration/rpc/test_integration_rpc \
  --gtest_filter='*Response*:*Channel*'
```

Expected: selected tests pass. Record exact counts. Replace any zero-match
filter with names from `--gtest_list_tests` before production edits.

- [ ] **Step 6: Extend the lifetime/dataflow inventory**

Use codebase-memory graph tools first, then literal search to classify:

- every encoded-frame producer and frame-handler byte shape;
- every `WireFrame::decode()` caller;
- every payload oneof/flag classifier;
- every RPC, reliable, backpressure, batch, and stream handler;
- every stream map/counter access and caller thread;
- every transport callback capture and setter propagation path;
- router/stream/messaging/RPC/transport outlives edges; and
- shutdown order from ingress disable through thread join and component
  destruction.

```bash
rg -n "WireFrame::decode|payload_type\(|data_frame\(\)\.flags" src include
rg -n "stream_senders_|stream_receivers_|stream_counter_|deliver_remote_stream" \
  src include
rg -n "set_.*handler|on_frame_received|frame_handler_" src/net include/hpactor/net
```

Update
`docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`, then:

```bash
git add docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "docs: inventory inbound frame and stream ownership"
```

---

### Task 1: Add strict typed `WireFrame` decode

**Deliverable:** Production can distinguish every framing/protobuf failure while
the existing `decode()` wrapper remains source-compatible.

**Files:**

- Create: `include/hpactor/net/frame_dispatch_result.hpp`
- Create: `tests/unit/net/test_frame_decode_result.cpp`
- Modify: `include/hpactor/msg/frame.hpp`
- Modify: `src/msg/frame.cpp`
- Modify: `tests/unit/net/CMakeLists.txt`

**Interfaces:**

- Produces:
  `FrameDecodeResult WireFrame::try_decode(const StreamBuffer&,
  FrameDecodeLimits = {})` and the span overload.
- Preserves: `WireFrame WireFrame::decode(...)` as a delegating compatibility
  wrapper.

- [ ] **Step 1: Write the failing decode matrix**

Add table-driven tests for exact success, header too short, invalid magic,
payload above 16 MiB, declared length shorter/longer than bytes, trailing bytes,
invalid protobuf, valid empty envelope, and future/unknown protobuf fields.

The core assertion shape is:

```cpp
auto result = net::WireFrame::try_decode(bytes, limits);
EXPECT_EQ(result.error, expected_error);
EXPECT_EQ(result.ok(), expected_error == net::FrameDecodeError::None);
```

Also assert `decode(invalid).payload_type() == PayloadType::Unknown` to preserve
compatibility.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_net
./build/tests/unit/net/test_unit_net \
  --gtest_filter='FrameDecodeResultTest.*'
```

Expected: compile failure because `try_decode` and result types do not exist.

- [ ] **Step 3: Add fixed decode types**

Implement exactly:

```cpp
enum class FrameDecodeError : uint8_t {
    None,
    HeaderTooShort,
    InvalidMagic,
    FrameTooLarge,
    LengthMismatch,
    TrailingBytes,
    InvalidProtobuf,
};

struct FrameDecodeLimits {
    uint32_t max_payload_bytes{16U * 1024U * 1024U};
    bool reject_trailing_bytes{true};
};
```

Define `FrameDecodeResult` after `WireFrame` is complete so it can own the
decoded value without pointers.

In `frame_dispatch_result.hpp`, also define the complete
`FrameDispatchCode`/`FrameDispatchResult` fixed-size types from the Phase 4
design. Later tasks consume those exact names; no temporary integer status API
is introduced.

- [ ] **Step 4: Implement validation before protobuf allocation**

Perform checked `HeaderSize + payload_len` arithmetic, enforce the configured
limit, require exact bytes when `reject_trailing_bytes`, and parse directly from
the bounded payload span. Do not first copy attacker-declared bytes into an
unbounded `std::string`.

- [ ] **Step 5: Delegate compatibility decode**

```cpp
WireFrame WireFrame::decode(const StreamBuffer& data) {
    auto decoded = try_decode(data);
    return decoded.ok() ? std::move(decoded.frame) : WireFrame{};
}
```

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_net test_integration_net
./build/tests/unit/net/test_unit_net \
  --gtest_filter='FrameDecodeResultTest.*:*WireFrame*'
./build/tests/integration/net/test_integration_net \
  --gtest_filter='*WireFrame*'
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/net/frame_dispatch_result.hpp \
  include/hpactor/msg/frame.hpp src/msg/frame.cpp tests/unit/net
git commit -m "refactor: add typed wire frame decode"
```

---

### Task 2: Normalize connection framing and bound event-loop work

**Deliverable:** Plain and TLS connections emit one complete canonical HPAC
frame, reject oversize lengths before allocation, and never recurse to resync.

**Files:**

- Modify: `include/hpactor/net/connection.hpp`
- Modify: `include/hpactor/net/wireframe_connection.hpp`
- Modify: `src/net/wireframe_connection.cpp`
- Modify: `include/hpactor/net/tls_connection.hpp`
- Modify: `src/net/tls_connection.cpp`
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/tcp_transport.cpp`
- Create: `tests/integration/net/test_inbound_frame_framing.cpp`
- Modify: `tests/integration/net/CMakeLists.txt`

**Interfaces:**

- Produces connection callbacks:
  `frame_handler(StreamBuffer canonical_encoded_frame)` and
  `frame_error_handler(FrameDecodeError, uint32_t observed_bytes)`.
- Consumes `FrameDecodeError` from Task 1.

- [ ] **Step 1: Write failing socket-level tests**

Using non-blocking socket pairs and condition-based polling, test:

- fragmented header and body;
- two complete frames in one read;
- plain and TLS handler bytes equal the original `WireFrame::encode()` bytes;
- invalid magic followed by a valid frame resynchronizes without recursion;
- advertised 16 MiB + 1 is rejected before payload reservation;
- EOF mid-frame reports `LengthMismatch`; and
- framing callback does not run for an incomplete frame.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_net
./build/tests/integration/net/test_integration_net \
  --gtest_filter='InboundFrameFramingTest.*'
```

Expected: plain callback lacks the HPAC header and oversize/error assertions
fail.

- [ ] **Step 3: Add the framing-error callback**

Store one callback next to `frame_handler_`. Setters are startup-only, matching
the existing frame handler publication rule. The error callback receives only
the typed reason and bounded byte count; it does not retain input bytes.

Add `uint32_t max_inbound_frame_bytes{16U * 1024U * 1024U}` to `PoolConfig` and
propagate it from `TcpTransport` into every accepted/connected plain or TLS
connection before read callbacks are enabled. Existing aggregate/default
construction remains source-compatible.

- [ ] **Step 4: Rewrite plain parsing as an iterative state machine**

Use a bounded loop over `read_buffer_`. Before `reserve_tail(payload_len)`, check:

```cpp
if (payload_len > max_inbound_frame_bytes_) {
    report_frame_error(FrameDecodeError::FrameTooLarge);
    close();
    return;
}
```

Extract `[begin, begin + HeaderSize + payload_len)` so the callback receives the
header. Replace recursive `handle_read()` calls with loop continuation and cap
resync bytes processed per event-loop turn.

- [ ] **Step 5: Normalize TLS plaintext**

After decrypting, feed plaintext through the same canonical HPAC boundary
validation, rather than calling the pool with an unspecified decrypted shape.
Apply the 16 MiB limit before outer or inner payload allocation.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_integration_net
./build/tests/integration/net/test_integration_net \
  --gtest_filter='InboundFrameFramingTest.*:*WireFrameConnection*:*TlsConnection*'
```

Expected: all pass with no sleeps or thread-order assumptions.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/net src/net tests/integration/net
git commit -m "fix: normalize bounded inbound framing"
```

---

### Task 3: Add the fixed unified inbound sink

**Deliverable:** Existing pools/transport can route every decode outcome through
one allocation-free sink, while direct legacy handler users retain fallback
behavior.

**Files:**

- Create: `include/hpactor/net/inbound_frame_sink.hpp`
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/connection_pool.cpp`
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`
- Modify: `tests/integration/net/test_inbound_frame_framing.cpp`

**Interfaces:**

- Produces `InboundFrameContext`, `InboundFrameSink`,
  `ConnectionPool::set_inbound_frame_sink()`, and
  `TcpTransport::set_inbound_frame_sink()`.
- Sink callbacks return the Task 1 `FrameDispatchResult` fixed value.

- [ ] **Step 1: Write failing sink precedence/propagation tests**

Assert:

- valid frame invokes unified `route` exactly once with pool peer and size;
- strict decode failure invokes `decode_failed` exactly once;
- connection framing error reaches `decode_failed` with peer;
- RPC, Data, Ack, Nack, Batch, Stream, and Unknown all use unified sink;
- when unified sink is present, legacy RPC/spawn/actor handlers receive zero
  calls; and
- without unified sink, current legacy split behavior remains.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_net
./build/tests/integration/net/test_integration_net \
  --gtest_filter='InboundFrameSinkTest.*'
```

Expected: compile failure because sink APIs do not exist.

- [ ] **Step 3: Implement the fixed sink value**

```cpp
struct InboundFrameSink {
    void* context{nullptr};
    FrameDispatchResult (*route)(void*, const InboundFrameContext&,
                                 const WireFrame&) noexcept{nullptr};
    FrameDispatchResult (*decode_failed)(void*, const InboundFrameContext&,
                                         FrameDecodeError) noexcept{nullptr};
};
```

An empty sink is detected by null function pointers. Do not use a virtual
interface or allocate a handler per frame.

- [ ] **Step 4: Make pool dispatch mutually exclusive**

`ConnectionPool::on_frame_received()` calls `try_decode()`. If unified sink is
present, route valid/error and return. Only an absent sink enters the preserved
legacy RPC/spawn/actor branch. Remove no legacy public setter in this task.

- [ ] **Step 5: Propagate to existing and future pools**

`TcpTransport::set_inbound_frame_sink()` updates pre-start pools under the
existing transport synchronization and stores the value for future pool
creation. Install connection framing-error callbacks that forward to their
pool.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_integration_net test_integration_rpc
./build/tests/integration/net/test_integration_net \
  --gtest_filter='InboundFrameSinkTest.*:*TransportRpcHandler*'
./build/tests/integration/rpc/test_integration_rpc \
  --gtest_filter='*Response*:*Channel*'
```

Expected: unified and fallback modes pass without double dispatch.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/net src/net tests/integration/net \
  tests/integration/rpc
git commit -m "refactor: add unified inbound frame sink"
```

---

### Task 4: Implement router classification, ordinary data, RPC, and failures

**Deliverable:** One concrete router classifies by oneof first and routes
ordinary/RPC frames with typed outcomes.

**Files:**

- Create: `src/net/inbound_frame_router.hpp`
- Create: `src/net/inbound_frame_router.cpp`
- Create: `src/actor/stream_runtime.hpp`
- Create: `src/actor/stream_runtime.cpp`
- Create: `tests/unit/net/test_inbound_frame_router.cpp`
- Modify: `src/net/CMakeLists.txt`
- Modify: `src/actor/CMakeLists.txt`
- Modify: `tests/unit/net/CMakeLists.txt`
- Modify: `src/runtime/actor_system_impl.hpp`
- Modify: `src/runtime/actor_system_impl.cpp`

**Interfaces:**

- Consumes Phase 3 `MessagingRuntime::try_deliver()` and
  `RpcChannel::on_response()`.
- Produces `InboundFrameRouter::route()`, `on_decode_failure()`, `disable()`,
  and `inbound_sink()`.

- [ ] **Step 1: Write failing classification tests**

Cover valid ordinary Data, RpcResponse Data, valid empty/unknown envelope,
invalid receiver, conflicting RPC/control flags, invalid trace context, decode
failure, and disabled router. Assert exact `FrameDispatchCode`, payload type,
counts, and downstream call count.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_net
./build/tests/unit/net/test_unit_net \
  --gtest_filter='InboundFrameRouterTest.*'
```

Expected: compile failure because the router does not exist.

- [ ] **Step 3: Implement concrete dependencies and accepting gate**

First add the concrete `StreamRuntime` ownership shell with immutable endpoint,
limits, messaging reference, and narrow actor/network ports, but no session map
or frame handlers yet. This is the final object type and construction contract,
not a nullable router dependency. Tasks 7 and 8 add its independently tested
registry and protocol behavior.

Define `StreamActorLifecyclePort` with the exact `spawn_sender`,
`spawn_receiver`, and `stop` function/context signatures from the design.
Define `StreamWirePort` with
`bool send(void*, const ActorAddress&, const net::WireFrame&) noexcept`.
Neither port captures the facade.

```cpp
struct Dependencies {
    runtime::MessagingRuntime& messaging;
    StreamRuntime& streams;
    RpcChannel& rpc;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
};
```

Store references and immutable config. `disable()` performs a release store;
`route()` begins with an acquire load and returns `RuntimeStopping` when false.

- [ ] **Step 4: Classify by oneof before fields**

Use a single switch over `frame.payload_type()`. Only the Data case reads
`data_frame()`. Unknown returns `UnsupportedPayload`. Ack/Nack/Batch/Stream may
initially return `HandlerUnavailable` until their tasks implement the branches;
they must not fall into Data.

- [ ] **Step 5: Route ordinary and RPC data**

Factor a helper that validates flags/address, builds `TypedMessage`, parses
trace with the configured limit, carries request-only ACK/message id, and calls
full messaging delivery. RpcResponse builds the existing `RpcResponseFrame` and
calls `rpc.on_response()` once.

- [ ] **Step 6: Bind a fixed sink**

Return function pointers whose context is the stable router object. The static
adapters call `route()`/`on_decode_failure()` and contain no facade capture.

- [ ] **Step 7: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_net test_integration_actor \
  test_integration_rpc
./build/tests/unit/net/test_unit_net \
  --gtest_filter='InboundFrameRouterTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*RemoteDelivery*'
./build/tests/integration/rpc/test_integration_rpc \
  --gtest_filter='*Response*:*Channel*'
```

Expected: all implemented branches pass and unimplemented typed branches fail
closed as `HandlerUnavailable`.

- [ ] **Step 8: Commit**

```bash
git add src/net src/actor src/runtime tests/unit/net tests/integration/actor \
  tests/integration/rpc
git commit -m "refactor: add inbound frame router"
```

---

### Task 5: Route reliable control and backpressure without flag ambiguity

**Deliverable:** Ordinary reliable requests are delivered, legacy responses are
unambiguous/rolling-compatible, and dedicated ACK/NACK/backpressure reach typed
messaging handlers.

**Files:**

- Modify: `src/net/inbound_frame_router.cpp`
- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/unit/net/test_inbound_frame_router.cpp`
- Modify: `tests/integration/mailbox/test_reliable_messaging.cpp`
- Modify: `tests/integration/actor/test_remote_backpressure_signals.cpp`

**Interfaces:**

- Consumes `MessagingRuntime::on_reliable_ack`, `on_reliable_nack`, and typed
  `on_remote_backpressure`.
- Produces explicit legacy flag and protobuf NackReason mapping helpers.

- [ ] **Step 1: Write failing reliable/control matrix**

Test:

- Data `AckRequested` only reaches ordinary delivery with ACK metadata;
- Data both bits invokes ACK only;
- Data `AckResponse` only invokes NACK only;
- RpcResponse plus either response-control bit is `InvalidFlags`;
- dedicated AckFrame invokes ACK;
- every known NackReason maps to expected canonical delivery status;
- unknown NackReason is `InvalidControlPayload`;
- valid/invalid backpressure payload produces handled/invalid result; and
- no control frame reaches an actor mailbox.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_net test_integration_mailbox
./build/tests/unit/net/test_unit_net \
  --gtest_filter='InboundFrameRouterReliableTest.*:*Backpressure*'
```

Expected: request-only data is currently consumed as ACK or typed branches are
unimplemented.

- [ ] **Step 3: Implement exact legacy flag table**

Use masks, not branch coincidence:

```cpp
const bool ack_requested = (flags & WireFrame::AckRequested) != 0;
const bool ack_response = (flags & WireFrame::AckResponse) != 0;
if (ack_requested && ack_response) return route_legacy_ack(data);
if (ack_response) return route_legacy_nack(data);
// ack_requested alone remains ordinary data metadata
```

- [ ] **Step 4: Make outgoing legacy ACK rolling-compatible**

For accepted/duplicate responses set both bits. For rejected responses set only
`AckResponse`. Preserve the current type/status and retry payload until a
separate dedicated-oneof migration.

- [ ] **Step 5: Implement dedicated mappings**

Read `ack_frame()`/`nack_frame()` only in their switch branches. Use an explicit
`switch` from protobuf `NackReason` to canonical delivery status. Do not cast
between enums.

- [ ] **Step 6: Decode backpressure in router**

Convert wire payload to `BackpressureSignal`, validate it, and pass the typed
signal to messaging. Remove the Phase 3 transitional `WireFrame` handler after
all callers move.

- [ ] **Step 7: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_net test_integration_mailbox \
  test_integration_actor test_unit_msg
./build/tests/unit/net/test_unit_net \
  --gtest_filter='InboundFrameRouterReliableTest.*:*Backpressure*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='*Reliable*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*RemoteBackpressure*:*RemoteDelivery*'
./build/tests/unit/msg/test_unit_msg \
  --gtest_filter='*Ack*:*Nack*:*OutboundDeliveryTracker*'
```

Expected: all pass and request-only data is observed by its actor.

- [ ] **Step 8: Commit**

```bash
git add src/net src/runtime tests/unit/net tests/unit/msg \
  tests/integration/mailbox tests/integration/actor
git commit -m "fix: disambiguate inbound reliable control"
```

---

### Task 6: Implement bounded ordered batch routing

**Deliverable:** Batch entries use the ordinary full-policy path and report
aggregate partial outcomes without atomicity claims.

**Files:**

- Modify: `src/net/inbound_frame_router.hpp`
- Modify: `src/net/inbound_frame_router.cpp`
- Modify: `tests/unit/net/test_inbound_frame_router.cpp`
- Modify: `tests/integration/actor/test_batch_messaging.cpp`
- Create: `tests/integration/actor/test_inbound_frame_routing.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

**Interfaces:**

- Consumes the Task 4 ordinary message builder/full messaging delivery helper.
- Produces Batch `FrameDispatchResult` counts and codes.

- [ ] **Step 1: Write failing batch tests**

Cover empty batch, one entry, ordered multi-entry, per-entry trace/message id/
flags, missing receiver, 1024 entries accepted, 1025 rejected before delivery,
one invalid entry, and mixed accepted/mailbox-rejected entries.

Assert that the router continues after a rejection and returns:

```cpp
EXPECT_EQ(result.code, FrameDispatchCode::BatchPartiallyDelivered);
EXPECT_EQ(result.accepted_count, 2u);
EXPECT_EQ(result.rejected_count, 1u);
EXPECT_EQ(result.invalid_count, 0u);
```

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_net test_integration_actor
./build/tests/unit/net/test_unit_net \
  --gtest_filter='InboundFrameRouterBatchTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='BatchMessagingTest.*:*InboundBatch*'
```

Expected: Batch returns `HandlerUnavailable` or falls through without delivery.

- [ ] **Step 3: Enforce bounds before iteration**

Reject `entries_size() > config_.max_batch_entries` with a fixed detail code
and zero downstream calls. Do not reserve a vector proportional to the remote
count.

- [ ] **Step 4: Route each entry through full messaging**

Build each `TypedMessage` from the common sender/receiver and entry metadata.
Use default priority/deadline because the wire schema has no such fields.
Preserve input iteration order and call the same full messaging method used by
ordinary Data.

- [ ] **Step 5: Aggregate without rollback**

Increment fixed counters. Return `BatchDelivered` only if all valid entries are
accepted; otherwise `BatchPartiallyDelivered` or `ActorRejected`. Do not remove
already accepted mailbox entries.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_net test_integration_actor
./build/tests/unit/net/test_unit_net \
  --gtest_filter='InboundFrameRouterBatchTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='BatchMessagingTest.*:*InboundBatch*'
```

Expected: all pass, including metadata and partial outcomes.

- [ ] **Step 7: Commit**

```bash
git add src/net tests/unit/net tests/integration/actor
git commit -m "feat: route bounded inbound batches"
```

---

### Task 7: Introduce the bounded peer-qualified `StreamRuntime` registry

**Deliverable:** One owner provides race-free registration, opening reservation,
lookup, terminal removal, id allocation, and registry-only snapshots.

**Files:**

- Create: `include/hpactor/actor/stream_snapshot.hpp`
- Modify: `src/actor/stream_runtime.hpp`
- Modify: `src/actor/stream_runtime.cpp`
- Create: `tests/unit/actor/test_stream_runtime.cpp`
- Modify: `src/actor/CMakeLists.txt`
- Modify: `tests/unit/actor/CMakeLists.txt`

**Interfaces:**

- Produces `StreamKey`, `StreamSession`, `StreamRuntime::reserve_open`,
  `commit_open`, `find_sender`, `find_receiver`, `remove_terminal`,
  `allocate_stream_id`, `snapshot`, and fixed `StreamDispatchResult`.
- Consumes a fixed `StreamActorLifecyclePort`, Phase 3 `MessagingRuntime&`, and
  fixed stream wire port.

- [ ] **Step 1: Write failing registry tests**

Test same id/different peers, exact duplicate, capacity 4096/4097, opening
counts toward capacity, generation-checked commit/rollback, spawn failure
cleanup, terminal remove exactly once, monotonic nonzero ids, counter wrap
collision retry, and bounded snapshot fields.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='StreamRuntimeRegistryTest.*'
```

Expected: compile failure because the registry/session APIs do not exist.

- [ ] **Step 3: Implement peer-qualified key and one session map**

```cpp
struct StreamKey {
    EndPoint peer;
    uint64_t stream_id{0};
    friend bool operator==(const StreamKey&, const StreamKey&) = default;
};

struct StreamSession {
    StreamSessionState state{StreamSessionState::Opening};
    ActorId sender_actor{};
    ActorId receiver_actor{};
    ActorId target_actor{};
    uint64_t generation{0};
};
```

Use one mutex and one `unordered_map<StreamKey, StreamSession, StreamKeyHash>`.
Define `StreamDispatchCode` with `Handled`, `UnknownStream`, `DuplicateStream`,
`CapacityExceeded`, `InvalidPeer`, `InvalidFrame`, `SpawnFailed`,
`DeliveryRejected`, and `MetadataDropped`. `StreamDispatchResult` carries that
code plus fixed accepted/rejected counts. Task 9 maps it to the net dispatch
result.

- [ ] **Step 4: Implement two-phase open reservation**

Under lock: reject zero id, duplicate, or capacity; insert Opening with a new
generation. Unlock for actor spawn. Re-lock and commit only if key/generation
still match. On mismatch, unlock and stop the late actor through the lifecycle
port.

- [ ] **Step 5: Implement lookup/removal/snapshot lock discipline**

Each lookup copies actor ids/session values. `remove_terminal` copies and erases
under lock, then returns the copy. `snapshot` copies at most 4096 registry
records and performs no actor lookup or formatting under lock.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='StreamRuntimeRegistryTest.*'
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/actor/stream_snapshot.hpp src/actor tests/unit/actor
git commit -m "refactor: add bounded stream runtime registry"
```

---

### Task 8: Move and correct stream protocol handlers

**Deliverable:** Stream open/data/ACK/close/error handlers use correct protobuf
messages/internal tags, typed failures, peer validation, and no callbacks under
the registry lock.

**Files:**

- Modify: `src/actor/stream_runtime.hpp`
- Modify: `src/actor/stream_runtime.cpp`
- Modify: `tests/unit/actor/test_stream_runtime.cpp`
- Create: `tests/integration/actor/test_stream_runtime_concurrency.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

**Interfaces:**

- Produces `StreamRuntime::on_open/on_data/on_ack/on_close/on_error`, each
  returning fixed `StreamDispatchResult`.
- Consumes `MessagingRuntime::try_deliver_fast(...,
  FastDeliveryReason::StreamProtocol)`.

- [ ] **Step 1: Write failing protocol mapping tests**

Assert:

- open rejects zero id, spoofed sender peer, duplicate, capacity, and missing
  local target;
- data uses `StreamDataTag` and protobuf-aware payload;
- ACK uses `StreamAckTag` and preserves last-sequence/window;
- close uses `StreamCloseTag` and preserves reason;
- error uses `StreamWireErrorTag` and preserves bounded code/description;
- unknown key returns `UnknownStream` and performs zero delivery;
- close/error delivers to copied sender/receiver ids and erases once; and
- all fast calls carry `StreamProtocol` reason.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='StreamRuntimeProtocolTest.*'
```

Expected: handlers are absent or current raw protobuf/tag behavior fails.

- [ ] **Step 3: Implement open validation/reservation/spawn/commit**

Validate `context.peer == from_proto(open.sender()).endpoint` when peer is
known. Reserve Opening, unlock, spawn receiver through the narrow port, then
commit. Parse trace with configured `max_tracestate_len`; invalid optional
trace is omitted and reported as a metadata warning.

- [ ] **Step 4: Implement protobuf-aware delivery helpers**

Construct messages as:

```cpp
TypedMessage message(stream::StreamAckTag, ack_frame);
return messaging_.try_deliver_fast(actor_id, std::move(message),
                                   FastDeliveryReason::StreamProtocol);
```

Use the corresponding internal tag/frame for each handler. Never copy a C++
protobuf object's memory representation.

- [ ] **Step 5: Implement terminal handling**

Copy and erase the session under lock. Unlock. Deliver the terminal protobuf to
each nonzero protocol actor id exactly once. Report unknown/partial delivery
through fixed counters; do not recreate state.

- [ ] **Step 6: Add deterministic concurrency tests**

Use barriers/latches, not sleeps, for concurrent different-peer same-id
registration, lookup versus close, snapshot versus open/erase, and late spawn
commit after close. Assert the documented rule: a lookup linearized before
erase may finish one delivery; a lookup after erase is Unknown.

- [ ] **Step 7: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_actor test_integration_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='StreamRuntimeProtocolTest.*:StreamRuntimeRegistryTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='StreamRuntimeConcurrencyTest.*'
```

Expected: all pass deterministically.

- [ ] **Step 8: Commit**

```bash
git add src/actor tests/unit/actor tests/integration/actor
git commit -m "fix: route stream protocol through StreamRuntime"
```

---

### Task 9: Connect stream routing and migrate the facade

**Deliverable:** Router handles all stream oneofs; ActorSystem owns no stream
map/counter/handler and all public methods are compatibility forwards.

**Files:**

- Modify: `src/net/inbound_frame_router.cpp`
- Modify: `src/runtime/actor_system_impl.hpp`
- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/actor/test_inbound_frame_routing.cpp`
- Modify: `tests/unit/actor/test_stream_handle.cpp`
- Modify: `tests/unit/actor/test_stream_sender_actor.cpp`

**Interfaces:**

- Consumes all Task 8 stream handlers and Task 4 router sink.
- Produces final facade forwards and transport sink installation.

- [ ] **Step 1: Write failing end-to-end routing/facade tests**

Route StreamOpen/Data/Ack/Close/Error through `InboundFrameRouter` and assert the
expected `StreamRuntime` result. Assert `deliver_remote()` and
`deliver_remote_batch()` invoke one router path. Exercise every public stream
registration/id/open method and verify the runtime snapshot changes.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='InboundFrameRoutingTest.*:*StreamFacade*'
```

Expected: stream router branches are unavailable or old facade handlers/maps
still receive traffic.

- [ ] **Step 3: Route every stream oneof**

Add direct branches from router switch to `streams_.on_*`. Map
`StreamDispatchResult` to `FrameDispatchResult` without inspecting the stream
registry in the router.

- [ ] **Step 4: Compose in dependency order**

Construct `StreamRuntime` after Actor/Messaging/RPC dependencies and
`InboundFrameRouter` after StreamRuntime. Install `router.inbound_sink()` into
the existing `TcpTransport` before listen/event-loop run.

- [ ] **Step 5: Replace facade bodies**

`deliver_remote` and `deliver_remote_batch` construct a compatibility context,
call the router once, and preserve `void`. Stream methods forward to
`StreamRuntime`, translating internal `result<StreamHandle>` to existing
`optional` only at the public boundary.

- [ ] **Step 6: Remove old stream state and handlers**

Delete `stream_senders_`, `stream_receivers_`, `stream_counter_`, and all five
`deliver_remote_stream_*` methods from facade/runtime shell. Remove old manual
payload/tag logic in the same commit.

- [ ] **Step 7: Verify unsupported remote open is honest**

Keep the existing optional API returning `nullopt` when no route/capability
exists. Do not make `StreamHandle::write()` appear remote-capable.

- [ ] **Step 8: Run GREEN**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_net \
  test_unit_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='InboundFrameRoutingTest.*:*Stream*:*Batch*:*RemoteDelivery*'
./build/tests/integration/net/test_integration_net \
  --gtest_filter='InboundFrameSinkTest.*:InboundFrameFramingTest.*'
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='*Stream*'
```

Expected: all pass with one router and no facade stream state.

- [ ] **Step 9: Commit**

```bash
git add src/net src/runtime src/actor include/hpactor/actor tests
git commit -m "refactor: route facade frames through runtime components"
```

---

### Task 10: Connect bounded observability, CLI snapshots, and shutdown order

**Deliverable:** Operators see decode/dispatch/stream outcomes without payload
or actor-state exposure, and callback lifetime is explicitly safe.

**Files:**

- Modify: `include/hpactor/metrics/metrics_event.hpp`
- Modify: `src/net/inbound_frame_router.cpp`
- Modify: `src/actor/stream_runtime.cpp`
- Modify: `src/cli/commands/stream_commands.cpp`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/runtime/actor_system_impl.hpp`
- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `tests/integration/cli/test_cli_commands_workflow.cpp`
- Modify: `tests/integration/metrics/test_metrics_integration.cpp`
- Modify: `tests/integration/actor/test_shutdown_coordinator.cpp`

**Interfaces:**

- Consumes `FrameDispatchResult` and `StreamRuntimeSnapshot`.
- Produces bounded metric codes, sanitized logs, CLI list data, and verified
  disable/join/destroy order.

- [ ] **Step 1: Write failing observability tests**

Assert one metric/result for decode failure, unsupported payload, invalid flags,
partial batch, unknown stream, duplicate stream, and stream capacity. Verify
logs omit payload, chunk bytes, trace state, and remote error description.

- [ ] **Step 2: Write failing snapshot/CLI tests**

Create controlled registry entries, call the public read-only snapshot, and
render `stream/list`. Assert peer/id/state/sender/receiver/target values. Assert
the command does not access protocol actor window/in-flight mutable fields.

- [ ] **Step 3: Write failing shutdown lifetime test**

Block a controlled inbound callback, begin shutdown, release it, and assert:

1. router disables;
2. ingress/listener stops;
3. event-loop thread joins;
4. transport callbacks retire;
5. router and stream runtime destruct afterward.

Use latches/hooks, not sleeps.

- [ ] **Step 4: Run RED**

```bash
ninja -C build test_integration_actor test_integration_cli \
  test_integration_metrics
ctest --test-dir build -R \
  'Inbound.*Observability|Stream.*Cli|Inbound.*Shutdown' \
  --output-on-failure
```

Expected: missing observations/rows or lifetime ordering assertions fail.

- [ ] **Step 5: Emit bounded observations**

Use fixed enum codes and existing ring buffer/log infrastructure. Include peer,
payload type, encoded size, reason, and stream id only. Rate-limit repeated
malformed/unknown peer events through the existing logging policy; never add
remote strings as metric labels.

- [ ] **Step 6: Wire registry-only CLI snapshot**

Format a copied `StreamRuntimeSnapshot` after its lock is released. Replace the
empty table with registry columns. Mark window/in-flight unavailable rather
than reading actor state cross-thread.

- [ ] **Step 7: Fix stop ordering**

Disable router before stopping ingress; stop/join event loop before destroying
router/stream/messaging/RPC/actor dependencies. Clear fixed sinks only after no
connection callback can run.

- [ ] **Step 8: Run GREEN**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_cli \
  test_integration_metrics test_integration_net
ctest --test-dir build -R \
  'Inbound|Stream|FrameDispatch|Shutdown' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/metrics include/hpactor/actor src/net src/actor \
  src/runtime src/cli tests
git commit -m "feat: expose bounded frame and stream observability"
```

---

### Task 11: Enforce architecture boundaries

**Deliverable:** Automated checks prevent frame classification and stream state
from returning to `ActorSystem` or `ConnectionPool`.

**Files:**

- Create: `tests/architecture/assert_frame_stream_routing_boundaries.cmake`
- Modify: `tests/architecture/CMakeLists.txt`
- Modify: production source only for violations discovered by the checks.

**Interfaces:**

- Consumes final source layout.
- Produces CTest architecture assertions.

- [ ] **Step 1: Add failing checks**

Enforce:

- production payload-type switches occur only in `InboundFrameRouter` and
  `WireFrame::payload_type()`;
- unified `ConnectionPool` mode contains no RPC/ACK/NACK/backpressure/batch/
  stream business handling;
- no network callback captures `ActorSystem` or `Impl`;
- stream map/counter construction occurs only in `StreamRuntime`;
- no `deliver_remote_stream_*` production method remains;
- direct `LocalDeliveryEngine` calls remain inside `MessagingRuntime`;
- stream fast delivery callers match the reviewed runtime/actor allowlist;
- facade frame/stream methods are forwards;
- no new `NetworkRuntime`, service locator, RTTI, exception flow, or public
  runtime header exists; and
- framing parser has no recursive self-call.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target test_architecture
ctest --test-dir build -R 'architecture.*frame.*stream' --output-on-failure
```

Expected: any remaining old classifier/map/capture fails.

- [ ] **Step 3: Remove violations with narrow allowlists**

Classify test fixtures and compatibility declarations separately. Do not
allowlist production policy copies or facade-capturing callbacks.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build build --target test_architecture
ctest --test-dir build -R 'architecture' --output-on-failure
```

Expected: all architecture checks pass.

- [ ] **Step 5: Commit**

```bash
git add tests/architecture src include/hpactor
git commit -m "test: enforce frame and stream routing boundaries"
```

---

### Task 12: Verify compatibility, concurrency, lifecycle, and documentation

**Deliverable:** Complete normal/socket/stress/sanitizer evidence, updated
lifetime memory, and a clean Phase 5 handoff.

**Files:**

- Modify: `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`
- Modify: `CLAUDE_MEMORY.md`
- Modify: source/tests only for defects proven by verification.

**Interfaces:**

- Consumes the complete Phase 4 implementation.
- Produces release/PR evidence and documented Phase 5 ownership handoff.

- [ ] **Step 1: Update final ownership/lifetime documentation**

Record canonical callback bytes, strict decode results, unified/legacy
precedence, router dependency graph, stream lock/linearization rules, bounded
defaults, callback stop order, facade compatibility, and the incomplete public
remote streaming non-goal.

- [ ] **Step 2: Run focused normal verification**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build hpactor_lib test_unit_net test_unit_actor test_unit_msg \
  test_integration_net test_integration_actor test_integration_mailbox \
  test_integration_rpc test_integration_cli test_integration_metrics \
  test_architecture
ctest --test-dir build -R \
  'unit_(net|actor|msg)|integration_(net|actor|mailbox|rpc|cli|metrics)|architecture' \
  --output-on-failure --parallel 8
```

Expected: all selected tests pass. Record exact count and elapsed time.

- [ ] **Step 3: Run deterministic stress/repeat coverage**

```bash
ctest --test-dir build -R \
  'InboundFrame|WireFrame|StreamRuntime|BatchMessaging|Reliable|Backpressure' \
  --repeat until-fail:50 --output-on-failure
```

Expected: no failure, unbounded allocation, recursion growth, or hang.

- [ ] **Step 4: Run ASan coverage**

```bash
cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-asan test_unit_net test_unit_actor test_integration_net \
  test_integration_actor test_integration_mailbox test_integration_rpc
ctest --test-dir build-asan -R \
  'FrameDecode|InboundFrame|StreamRuntime|Batch|Reliable|Shutdown' \
  --output-on-failure
```

Expected: no overflow, use-after-free, leak, late callback, or orphan opening
actor.

- [ ] **Step 5: Run TSAN coverage**

```bash
cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-tsan test_integration_net test_integration_actor \
  test_integration_mailbox
ctest --test-dir build-tsan -R \
  'InboundFrame|StreamRuntime|Reliable|Backpressure|Shutdown' \
  --output-on-failure
```

Expected: no race in sink publication, router disable, stream registry,
terminal removal, snapshot, tracker control, or shutdown.

- [ ] **Step 6: Run full verification because transport/public headers and
  cross-cutting ingress changed**

```bash
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
```

Expected: full build and suite pass. Record exact count and elapsed time.

- [ ] **Step 7: Verify wire/source compatibility explicitly**

Run existing frame roundtrip, ACK/NACK protobuf, trace propagation, batch
encode, direct transport handler, and public stream-handle tests. Compare
encoded protobuf field numbers/oneof cases and TypeTag constants against the
baseline. Demonstrate old ACK-first classifier behavior accepts the new
dual-bit legacy ACK.

- [ ] **Step 8: Review Phase 5 boundary**

```bash
git diff origin/main...HEAD --stat
rg -n "class NetworkRuntime|unique_ptr<.*EventLoop|network_thread_" \
  src/net src/runtime include/hpactor
git diff origin/main...HEAD -- src/net src/actor src/runtime \
  include/hpactor/net include/hpactor/actor
```

Expected: no new network lifecycle owner. Existing shell ownership remains;
router and stream policy are extracted.

- [ ] **Step 9: Update project memory and commit docs**

Document exact evidence, corrected defects, bounded defaults, compatibility
rule, incomplete streaming non-goal, and Phase 5 handoff.

```bash
git add docs/architecture/actor/actor-system-phase1-lifetime-inventory.md \
  CLAUDE_MEMORY.md
git commit -m "docs: record frame and stream routing ownership"
```

- [ ] **Step 10: Request review and verify clean state**

Invoke `superpowers:requesting-code-review`, address valid findings through
RED -> GREEN, then invoke `superpowers:verification-before-completion`.

```bash
git status --short
git log --oneline --decorate origin/main..HEAD
```

Expected: empty status and focused reviewable commits.

## Completion Evidence Checklist

- [ ] merged Phase 0-3 prerequisite references;
- [ ] baseline/final focused test counts;
- [ ] canonical plain/TLS callback byte evidence;
- [ ] strict decode error matrix and 16 MiB pre-allocation bound evidence;
- [ ] oneof/flag routing matrix, including dual-bit rolling compatibility;
- [ ] batch metadata/order/bound/partial-result evidence;
- [ ] peer-qualified stream collision, bound, rollback, and TypeTag evidence;
- [ ] unified-versus-legacy no-double-dispatch evidence;
- [ ] operations snapshot and payload-redaction evidence;
- [ ] architecture checks;
- [ ] ASan/TSAN/stress results;
- [ ] full build/test count; and
- [ ] confirmation that network lifecycle ownership remains Phase 5.

## Final Acceptance Checklist

- [ ] Plain/TLS ingress emits one canonical complete HPAC frame shape.
- [ ] Framing is bounded/iterative and strict decode failures are typed.
- [ ] Unified sink has exclusive precedence with legacy fallback preserved.
- [ ] One router classifies every valid payload oneof.
- [ ] Ordinary reliable requests are not consumed as ACK responses.
- [ ] Legacy and dedicated ACK/NACK map explicitly to messaging control.
- [ ] Batch delivery is ordered, bounded, metadata-preserving, full-policy,
      and partial-result aware.
- [ ] Stream state is one peer-qualified bounded registry under one owner.
- [ ] Stream callbacks never run under the registry mutex.
- [ ] Stream protocol uses protobuf-aware internal TypeTags.
- [ ] Unknown/duplicate/capacity/invalid outcomes are typed and observable.
- [ ] Facade frame/stream methods are forwards and contain no state/policy.
- [ ] Callback disable/join/destruction order is sanitizer-tested.
- [ ] CLI/admin snapshots contain registry facts only.
- [ ] Remote `StreamHandle` completion is neither implemented nor claimed.
- [ ] No `NetworkRuntime` or Phase 5 ownership entered this branch.
