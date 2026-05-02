# Unified Message Passing Architecture

## Overview

HPActor actors communicate through a unified message passing system that provides **location transparency** — the sender uses the same API regardless of whether the target actor lives in the same process or on a remote host. The system automatically selects the optimal transport path:

- **Same process**: Zero-copy `shared_ptr<Message>` handoff via lock-free MPSC mailbox
- **Cross-host**: Protobuf serialization over TCP, dispatched by `ActorId` in the message header

**Core Principle:** `ActorContext::send()` and `ActorRef::send()` are the single entry points. Location is an optimization detail, not an API concern.

---

## Architecture

### Sending Path

```
User Code
  │
  ├─ context()->send(target, msg)
  └─ context()->reply(msg)
       │
       ▼
┌─────────────────────────────────────────────────────────────┐
│                      ActorContext                            │
│                                                             │
│  ref_cache_: unordered_map<ActorId, ActorRef>               │
│  current_sender_: ActorAddress   // set on receive          │
│                                                             │
│  resolve(target) → ActorRef                                  │
│    ├─ check ref_cache_ (hot path)                           │
│    ├─ check local actor registry via system_                │
│    ├─ create ActorProxy for non-loopback endpoints          │
│    └─ populate cache, return ActorRef                       │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│               ActorRef (variant<Actor, ActorProxy>)          │
│                                                             │
│  send(target, msg) {                                        │
│    if (is_local()) {                                        │
│      actor->system().deliver_local(target.id, msg);         │
│    } else {                                                 │
│      proxy->send(target, msg);                              │
│    }                                                        │
│  }                                                          │
└──────────────────────────┬──────────────────────────────────┘
                           │
              ┌────────────┴────────────┐
              ▼                         ▼
┌──────────────────────┐   ┌──────────────────────────────────┐
│   LOCAL FAST PATH     │   │        REMOTE PATH               │
│                      │   │                                  │
│  deliver_local()     │   │  ActorProxy::send()              │
│       │              │   │       │                          │
│       ▼              │   │  WireFrame {                     │
│  get_mailbox(id)     │   │    sender:   ActorAddress        │
│       │              │   │    receiver: ActorAddress        │
│       ▼              │   │    msg_id:   uint64              │
│  mailbox->push(msg)  │   │    type_tag: uint32 (TypeTag)    │
│                      │   │    flags:    uint32              │
│  TypedMessage carries │   │    payload:  StreamBuffer        │
│  shared_ptr<Message>  │   │  }                               │
│  and sender_address_  │   │       │                          │
│  (zero-copy)          │   │  frame.encode()                  │
│                      │   │    → magic + length +             │
│                      │   │      protobuf (ActorMsgFrame)     │
│                      │   │  ConnectionPool::send()          │
│                      │   │       │                          │
│                      │   │  TCP socket write()              │
└──────────────────────┘   └──────────────────────────────────┘
```

### Receiving Path

```
┌─────────────────────────────────────────────────────────────┐
│                   TCP / ConnectionPool                       │
│                                                             │
│  on_frame_received(bytes)                                   │
│    → validate magic "HPAC"                                   │
│    → read length → extract protobuf payload                  │
│    → WireFrame::decode(bytes)                                │
│    → {sender, receiver, msg_id, type_tag, flags, payload}   │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                      ActorSystem                             │
│                                                             │
│  deliver_remote(frame)                                      │
│    → TypedMessage(type_tag, payload)                         │
│    → msg.set_sender_address(frame.sender)                    │
│    → deliver_local(receiver.id, msg)  // SAME sink          │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                 deliver_local(target_id, msg)                │
│                                                             │
│  mailbox = get_mailbox(target_id)                           │
│  mailbox->push(msg)                                         │
│                                                             │
│  This is the single, unified entry point for ALL inbound     │
│  messages — local and remote origin.                        │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                    RECEIVING ACTOR                           │
│                                                             │
│  EventBasedActor::receive(msg)                              │
│    │                                                        │
│    ├─ context->current_sender_ = msg.sender_address()      │
│    │   // enables reply() to work                           │
│    │                                                        │
│    ├─ dispatch by TypeTag:                                  │
│    │   System tags (1-6): link/unlink/exit/down/spawn       │
│    │   User tags (100+):  proto_handlers_[tag]              │
│    │                                                        │
│    └─ msg.as<T>() — lazy deserialize:                       │
│          local:  static_pointer_cast<T>(parsed_)  // free   │
│          remote: ParseFromArray(payload)           // once  │
└─────────────────────────────────────────────────────────────┘
```

---

## Key Components

### 1. TypedMessage — Universal Carrier

`TypedMessage` is the single message type flowing through all paths. It holds both representations:

| Field | Purpose | Local Path | Remote Path |
|-------|---------|------------|-------------|
| `tag_` (TypeTag) | Dispatch routing | Set by sender | Set by sender, encoded in frame |
| `payload_` (bytes) | Serialized protobuf | Eagerly serialized by sender | Transmitted over wire |
| `parsed_` (shared_ptr<Message>) | Zero-copy handle | Set by sender, used directly by receiver | nullptr on arrival, lazy-parsed on first `as<T>()` |
| `sender_address_` (ActorAddress) | Reply routing | Set by `ActorContext::send()` | Set by `deliver_remote()` from WireFrame.sender |

The `parsed_` field is `mutable` — the receiver lazily deserializes on first access, caches the result, and all subsequent accesses are zero-cost.

### 2. ActorRef — Unified Reference with Variant Dispatch

```
ActorRef = std::variant<Actor, ActorProxy>
```

- **Actor** holds `shared_ptr<AbstractActor>` — for local actors only
- **ActorProxy** holds `ActorAddress` + `Transport*` — for remote actors

`ActorRef::send()` is the dispatch point. It checks `is_local()` (whether the variant holds `Actor`) and routes accordingly. `ActorContext` maintains a cache of resolved `ActorRef` objects keyed by `ActorId`.

### 3. ActorContext — Resolution and Caching

`ActorContext` gains two new responsibilities:

**`resolve(ActorAddress) → ActorRef`:**
1. Check `ref_cache_` by `ActorId` — hot path for repeated sends
2. Query `system_->get_actor(id)` for local actors
3. For non-loopback endpoints: create an `ActorProxy` wrapping the transport
4. Insert into cache, evict LRU entries

**`current_sender_` tracking:**
Set to the sender's `ActorAddress` on each `receive()`. Enables `reply(msg)` to work without the caller specifying a target.

### 4. deliver_local() — Single Inbound Sink

`ActorSystem::deliver_local(ActorId, TypedMessage, priority, deadline)` is the **only** entry point for messages entering an actor's mailbox. Both local sends and remote receives flow through it.

```cpp
void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t priority = 0,
                                int64_t deadline_ns = INT64_MAX);
```

Sender identity is carried by the message itself (`msg.sender_address()`), set by the caller before `deliver_local()` is invoked. For local sends, `ActorContext::send()` sets it from `owner_.address()`. For remote receives, `deliver_remote()` sets it from `WireFrame::sender`. The scheduler model is unchanged — polling-based, no per-message `notify_ready()`.

### 5. ActorSystem::deliver_remote() — Wire-to-Mailbox Bridge

New method that connects the transport layer to the unified mailbox:

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    TypedMessage msg(static_cast<TypeTag>(frame.pb_frame.type_tag()),
                     std::move(payload));
    msg.set_sender_address(net::from_proto(frame.pb_frame.sender()));
    deliver_local(net::from_proto(frame.pb_frame.receiver()).id,
                  std::move(msg));
}
```

This replaces the current TODO in `ConnectionPool::on_frame_received()`.

---

## Wire Format

Every actor message on the wire is wrapped in a length-delimited framing envelope:

```
Wire format:
  [4 bytes: magic "HPAC" (0x48 0x50 0x41 0x43)]
  [4 bytes: remaining_length (uint32_t, network byte order)]
  [N bytes: protobuf-serialized ActorMsgFrame]
```

The magic header identifies the stream as HPACTOR protocol. The length prefix enables message boundary detection without parsing protobuf.

The protobuf message `ActorMsgFrame` (defined in `protos/hpactor/frame.proto`) is the frame body:

```protobuf
message ActorMsgFrame {
    PbActorAddress sender = 1;   // Who sent this message
    PbActorAddress receiver = 2; // Target actor address
    uint32 type_tag = 3;         // TypeTag enum for dispatch
    uint64 message_id = 4;       // Globally unique ID
    uint32 flags = 5;            // Important, NoDrop, RpcRequest, etc.
    bytes payload = 6;           // Already-serialized protobuf content
}
```

The `PbActorAddress` in the `receiver` field contains the target `ActorId` — the receiving node extracts it for mailbox lookup.

### Frame Flags

| Flag | Bit | Purpose |
|------|-----|---------|
| `Important` | 0x01 | Priority delivery hint |
| `NoDrop` | 0x02 | Guaranteed delivery (no mailbox overflow drop) |
| `RpcRequest` | 0x04 | This message is an RPC request |
| `RpcResponse` | 0x08 | This message is an RPC response |
| `RpcIdempotent` | 0x10 | Safe to retry |

---

## Reply Tracking

### Current State

`ActorContext::reply()` and `reply_with_error()` are TODO stubs with empty bodies. The sender of the current message is never captured.

### Design

On each `EventBasedActor::receive(msg)`:
1. Extract `sender` from the frame header (for remote) or from the `TypedMessage` metadata (for local)
2. Store in `context->current_sender_`
3. `reply(msg)` sends to `current_sender_` using the same unified `send()` path
4. `reply_with_error(err)` wraps the error in a standard error message and replies

For local messages, the sender's `ActorAddress` is attached to the `TypedMessage` before it's pushed into the mailbox. For remote messages, it comes from `WireFrame::pb_frame.sender()` and is converted via `net::from_proto()`.

---

## API Summary

### Sending (User-Facing)

```cpp
// Primary: send to an actor reference (local or remote)
context()->send(actor_ref, msg);

// Convenience: send to an address (lazy resolve + cache)
context()->send(actor_address, msg);

// Reply to the sender of the current message
context()->reply(msg);
context()->reply_with_error(err);

// Send with priority and deadline
context()->send_with_priority(target, msg, priority, deadline_ns);

// RPC (already implemented)
context()->rpc(target, encoded_request, timeout_ms);
```

### Sending (Internal)

```cpp
// Single unified mailbox entry point
ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                           uint8_t priority = 0,
                           int64_t deadline_ns = INT64_MAX);

// Wire-to-mailbox bridge (new)
ActorSystem::deliver_remote(const net::WireFrame& frame);
```

---

## Migration from Current State

| Current | Target | Change |
|---------|--------|--------|
| `ActorContext::send()` drops remote targets | Routes through `ActorRef` cache | Add `resolve()`, cache, remote path |
| `reply()` is empty TODO | Works via `current_sender_` | Wire sender tracking in `receive()` |
| `reply_with_error()` is empty TODO | Sends error message to sender | Same as reply, with error payload |
| `deliver_local()` ignores priority/deadline | Priority/deadline params accepted (forward-looking), sender carried by msg.sender_address_ | Update signature to include params |
| `on_frame_received()` only handles RPC/spawn | Routes all messages to `deliver_remote()` | Generalize dispatch |
| `ActorProxy::send()` works | Unchanged | Already correct |
