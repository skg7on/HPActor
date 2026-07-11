# Streaming Pipeline Demo (`15_streaming_pipeline`)

A comprehensive demonstration of the HPActor **MSG-008 credit-based streaming
message protocol** for long-running actor bulk data transfers.

## Overview

The streaming protocol provides session-oriented, flow-controlled channels
between actors. Unlike individual `send()` calls where every message traverses
the full `DeliveryPipeline`, a stream establishes a lightweight session with
credit-based byte-window flow control.

This demo covers the full API surface across two operating modes:

| Mode | Description | Threads |
|------|-------------|---------|
| **Local scenarios** (`--scenario`) | Single-process, same-node stream operations | 0 (deterministic) |
| **Cross-process** (`--mode`) | Two processes communicating via TCP loopback | > 0 (required) |

## Build

```bash
cmake -S . -B build -GNinja -DENABLE_APPS=ON
ninja -C build 15_streaming_pipeline
```

The binary is at `build/apps/streaming_pipeline/15_streaming_pipeline`.

---

## Local Scenarios

All local scenarios run in a single process with `scheduler_threads=0`.
They validate the `StreamHandle` API contracts and demonstrate actor patterns
for producing/consuming streams.

### Usage

```bash
# Run all scenarios (default)
./build/apps/streaming_pipeline/15_streaming_pipeline

# Run a specific scenario
./build/apps/streaming_pipeline/15_streaming_pipeline --scenario <name>

# Verbose per-operation output
./build/apps/streaming_pipeline/15_streaming_pipeline --scenario <name> --verbose
```

### Scenarios

#### `api-surface` — Full API walkthrough

Exercises every `StreamHandle` method:

1. `open_stream()` — valid and invalid targets
2. `is_open()` / `stream_id()` — introspection
3. `bytes_in_flight()` / `window_bytes()` — observability snapshots
4. `write(TypeTag, StreamBuffer)` — chunk with tag preservation
5. `write(TypedMessage)` — overload
6. `close()` — graceful close
7. Post-close guards — write/close/error all return `false`
8. `error(code, desc)` — abort with error code
9. Move semantics — moved-from handle is closed
10. `StreamConfig` variants — tiny (512 B) to large (1 MiB) windows
11. `StreamConfig` defaults

```
$ ./15_streaming_pipeline --scenario api-surface
  [PASS] api-surface
```

#### `pipeline` — Three-stage chain

```
SensorActor ──(SensorReadingTag)──► TransformActor ──(TransformedChunkTag)──► AnalyticsActor
```

The **TypeTag is preserved** through each stream hop: `SensorReadingTag` set at
`write()` time is recovered by the receiver as `SensorReadingTag`, not
`StreamChunkTag`. The TransformActor computes per-batch statistics (min, max,
mean, stddev) and forwards results downstream.

Actor wiring:
```cpp
auto analytics  = system.spawn<AnalyticsActor>(...);
auto transformer = system.spawn<TransformActor>(analytics.id(), ...);
transformer->open_downstream();  // opens stream to analytics
auto sensor = system.spawn<SensorActor>(transformer.id(), ...);
// Kick the sensor → streams data through the pipeline
```

#### `multi-stream` — Concurrent streams

Three `SensorActor` instances stream concurrently to a single `AnalyticsActor`.
Each stream has an independent `stream_id`, credit window, and lifecycle.
Closing one stream does not affect the others.

```
SensorActor-1 ──(stream 0xA...)──┐
SensorActor-2 ──(stream 0xB...)──┼── AnalyticsActor
SensorActor-3 ──(stream 0xC...)──┘
```

#### `flow-control` — Window sizing

Demonstrates how `StreamConfig` window parameters affect throughput:

| Configuration | `initial_window_bytes` | Expected behavior |
|---------------|----------------------|-------------------|
| Default | 64 KiB | Full throughput |
| Tiny | 256 B | Sender pauses frequently |
| Large | 1 MiB | High throughput |
| Frame-limited | 64 KiB, `max_in_flight_frames=8` | Frame-count capped |

#### `error-handling` — Close vs error semantics

| Operation | Receiver gets |
|-----------|--------------|
| `handle.close()` | `StreamClosedTag` (COMPLETE) |
| `handle.error(code, desc)` | `StreamErrorTag` (with error code and description) |
| `close()` after `close()` | `false` (idempotent guard) |
| `close()` after `error()` | `false` (terminal state) |
| `write()` after `close()` | `false` |

---

## Cross-Process Streaming

Two independent processes communicate via **TCP loopback** (`127.0.0.1`).
The sender opens a **remote stream** by calling `open_stream(ActorRef)` with
an `ActorRef` wrapping an `ActorProxy` pointed at the receiver's endpoint.

### Architecture

```
┌─ Sender Process ─────────────────────┐    ┌─ Receiver Process ────────────────────┐
│                                      │    │                                        │
│  Main thread                         │    │  Main thread                           │
│    │                                 │    │    │                                   │
│    │ transport->connect(receiver)    │    │    │ spawn<ReceiverActor>()            │
│    │ open_stream(remote_actor_ref)   │    │    │ poll for stream events            │
│    │ handle.write() × N              │    │    │                                   │
│    │ handle.close()                  │    │    │                                   │
│    ▼                                 │    │    ▼                                   │
│  StreamSenderActor (local)           │    │  Network thread (event loop)           │
│    │                                 │    │    ▲                                   │
│    │ try_send() via transport        │    │    │ incoming TCP connection           │
│    ▼                                 │    │    │ InboundFrameRouter.dispatch()     │
│  TcpTransport                        │    │    │ StreamRuntime.on_open()            │
│    │                                 │    │    │ StreamRuntime.on_data()           │
│    │ TCP frames ─────────────────────┼────┼────┘ StreamRuntime.on_close()          │
│    │                                 │    │    │                                   │
│    │ ◄─── StreamAck frames ──────────┼────┼──── (credit window updates)           │
│                                      │    │    ▼                                   │
│                                      │    │  StreamReceiverActor (spawned by       │
│                                      │    │    StreamRuntime on remote side)        │
│                                      │    │    │                                   │
│                                      │    │    │ deliver chunks to target actor    │
│                                      │    │    ▼                                   │
│                                      │    │  ReceiverActor                         │
│                                      │    │    receives: StreamOpenedTag            │
│                                      │    │             SensorReadingTag chunks     │
│                                      │    │             StreamClosedTag             │
└──────────────────────────────────────┘    └────────────────────────────────────────┘
```

### Wire protocol (remote path)

When `open_stream(ActorRef)` is called for a remote target:

1. A **local** `StreamSenderActor` is spawned in the sender process
2. A `StreamOpenFrame` (protobuf) is encoded into a `WireFrame` and sent via `TcpTransport::try_send()`
3. The receiver's event loop reads the frame, decodes it, and dispatches to `InboundFrameRouter`
4. `StreamRuntime::on_open()` spawns a `StreamReceiverActor` on the receiver side
5. `StreamOpenedTag` is delivered to the target actor
6. `StreamDataFrame`s carry chunk payloads; `StreamAckFrame`s carry credit window updates back
7. `StreamCloseFrame` / `StreamErrorFrame` terminate the session

### Step-by-step walkthrough

#### Step 1: Start the receiver

```bash
./15_streaming_pipeline --mode receiver --port 17130 --threads 4
```

Output:
```
=== HPActor Streaming Pipeline — Receiver ===
Port: 17130
Creating ActorSystem...
ActorSystem created.
Listening on: 127.0.0.1:17130
Spawning receiver actor...
Actor spawned.
Target ActorId: 3

>>> Run in another terminal:
    ./15_streaming_pipeline --mode sender --target-endpoint 127.0.0.1:17130 --target-id 3 --port 17131

Waiting for incoming stream... (Ctrl+C to stop)
```

The receiver:
- Creates an `ActorSystem` with `enable_network=true`, `tcp_port=17130`, `scheduler_threads=4`
- Binds TCP port 17130 and starts the network event loop
- Spawns a `ReceiverActor` that handles stream lifecycle events
- Prints the `ActorId` needed by the sender
- Polls until a stream completes or 60-second timeout

#### Step 2: Run the sender

In a **separate terminal**, run the command printed by the receiver:

```bash
./15_streaming_pipeline --mode sender \
    --target-endpoint 127.0.0.1:17130 \
    --target-id 3 \
    --port 17131 \
    --batches 50 \
    --threads 4 \
    --verbose
```

Output:
```
=== HPActor Streaming Pipeline — Sender ===
Target: 127.0.0.1:17130  ActorId: 3
Local endpoint: 1.0.0.127:0
Connecting to 127.0.0.1:17130...
Connected: yes
Opening remote stream to 127.0.0.1:17130 actor=3...
Remote stream opened: 0x300000000
  [10/50] in_flight=0B window=0B
  [20/50] in_flight=0B window=0B
  [30/50] in_flight=0B window=0B
  [40/50] in_flight=0B window=0B
  [50/50] in_flight=0B window=0B
Wrote 50/50 batches (500 readings)
Closing remote stream...
Stream closed. Check receiver output for results.
```

The sender:
- Creates an `ActorSystem` with `enable_network=true`, `tcp_port=17131`, `scheduler_threads=4`
- Establishes a TCP connection to `127.0.0.1:17130` via `transport->connect()`
- Constructs an `ActorProxy` from the target `ActorAddress` + `Transport`
- Wraps it in an `ActorRef` (location-transparent actor reference)
- Calls `system.open_stream(actor_ref, stream_config)` — this triggers the remote stream path
- Writes N batches via `handle.write()` — each chunk is tagged with `SensorReadingTag`
- Calls `handle.close()` to gracefully terminate

### CLI reference

| Flag | Mode | Default | Description |
|------|------|---------|-------------|
| `--mode receiver\|sender` | both | *(required)* | Cross-process operating mode |
| `--port <n>` | both | `0` (auto) | TCP port for transport |
| `--registrar-port <n>` | both | `0` | UDP registrar port (0 = auto-configure, avoids mDNS 5353) |
| `--target-endpoint <ip:port>` | sender | *(required)* | Receiver's TCP address (e.g. `127.0.0.1:17130`) |
| `--target-id <n>` | sender | *(required)* | Receiver actor's numeric `ActorId` |
| `--batches <n>` | sender | `50` | Number of sensor batches to stream |
| `--threads <n>` | both | `2` | Scheduler worker threads (must be > 0) |
| `--verbose, -v` | both | off | Per-operation progress output |

### Network requirements

For cross-process streaming to work:

1. **Both processes** must have `scheduler_threads > 0` (worker threads + network event loop)
2. **Both processes** must use different TCP ports on the same host
3. **Receiver** must bind its TCP port before the sender connects
4. **Sender** must know the receiver's `ActorId` (printed at receiver startup)
5. The UDP registrar port should be set explicitly (e.g. `--registrar-port 19153`) to avoid
   conflicts with mDNS (port 5353)

### Key code patterns

**Constructing a remote ActorRef:**

```cpp
// Parse the receiver's endpoint
auto target_ep = endpoint_ops::parse_endpoint("127.0.0.1:17130");

// Build the target's full address
ActorAddress target_addr(target_ep, ActorType{0}, ActorId{target_id}, 0);

// Establish TCP connection (required before try_send)
auto* transport = system.transport();
transport->connect(target_ep, "127.0.0.1", 17130);

// Wrap in an ActorProxy → ActorRef for location-transparent access
ActorProxy proxy(target_addr, transport);
ActorRef target_ref(std::move(proxy));

// open_stream(ActorRef) triggers the remote stream path
auto handle = system.open_stream(target_ref, stream_config);
```

**Receiving stream events:**

```cpp
class ReceiverActor : public EventBasedActor {
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            auto tag = msg.type_id();
            if (tag == stream::StreamOpenedTag) {
                // Stream session established — sender's ActorId is in
                // msg.sender_address()
            } else if (tag == stream::StreamChunkTag) {
                // Data chunk arrived — original TypeTag preserved
            } else if (tag == stream::StreamClosedTag) {
                // Graceful close — reason in StreamClosedPayload
            } else if (tag == stream::StreamErrorTag) {
                // Error abort — error_code and description in
                // StreamErrorPayload
            }
        }};
    }
};
```

---

## Stream Lifecycle TypeTags

| TypeTag | Value | Direction | When delivered |
|---------|-------|-----------|----------------|
| `StreamChunkTag` | `0x80` | Sender → Receiver | Each data chunk, with original user TypeTag preserved |
| `StreamOpenedTag` | `0x81` | Framework → Receiver | Stream session established |
| `StreamClosedTag` | `0x82` | Framework → Receiver | Stream gracefully closed (COMPLETE, CANCELLED, or TIMEOUT reason) |
| `StreamErrorTag` | `0x83` | Framework → Receiver | Stream aborted with error code and description |

**TypeTag preservation**: When `handle.write(MyTag, payload)` is called, the receiver
sees `MyTag`, not `StreamChunkTag`. The original tag is carried in the
`StreamDataFrame.user_tag` field and restored on delivery.

---

## StreamConfig Reference

```cpp
struct StreamConfig {
    uint32_t initial_window_bytes  = 65536;   // 64 KiB — receiver's initial credit window
    uint32_t max_chunk_bytes       = 65536;   // 64 KiB — max single chunk payload
    uint32_t send_buffer_bytes     = 262144;  // 256 KiB — sender's local buffer
    Duration idle_timeout          = 30s;     // stream errored if idle for this long
    uint32_t max_in_flight_frames  = 256;     // max unacknowledged data frames
};
```

Override at open time:
```cpp
StreamConfig cfg;
cfg.initial_window_bytes = 128 * 1024;  // 128 KiB window
cfg.idle_timeout = Duration::from_seconds(60);
auto handle = system.open_stream(target, cfg);
```
