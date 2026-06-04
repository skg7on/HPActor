# MSG-002: Result-Returning Delivery APIs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce `DeliveryStatus` / `DeliveryResult` as the unified user-facing delivery outcome type, extend transport to return `TransportSendResult` instead of `bool`, and change `try_send()` to return `DeliveryResult` across `ActorContext`, `ActorRef`, and `ActorProxy`.

**Architecture:** A new `delivery_result.hpp` header defines the two public types and constexpr mapping from internal `EnqueueResultCode` / `TransportSendResult` values. `EnqueueResult` gains a `to_delivery_result()` conversion. `Transport::try_send()` returns `TransportSendResult`. `ActorProxy::try_send()` maps transport result → `DeliveryResult`. `ActorContext` gains `try_reply()`. `ActorSystem` gains `deliver_with_result()`. All existing `void send()` / `void reply()` signatures remain untouched.

**Tech Stack:** C++20, CMake/Ninja, GoogleTest, HPActor mailbox/actor/transport/CLI runtime, no exceptions, no RTTI.

**Spec:** `docs/superpowers/specs/2026-06-04-msg002-result-delivery-api-design.md`

---

## Source Design Inputs

- Spec: `docs/superpowers/specs/2026-06-04-msg002-result-delivery-api-design.md`
- Architecture doc: `docs/architecture/production/actor-delivery-semantics-design.md`
- Existing delivery types: `include/hpactor/mailbox/mailbox_policy.hpp` (EnqueueResult, EnqueueResultCode)
- Existing delivery mode: `include/hpactor/mailbox/delivery_mode.hpp`
- Public API headers: `include/hpactor/actor_context.hpp`, `include/hpactor/ref/actor_ref.hpp`, `include/hpactor/ref/actor_proxy.hpp`
- Internal delivery: `include/hpactor/core/actor_system.hpp`, `src/actor/actor_system.cpp`
- Transport interface: `include/hpactor/net/transport.hpp`
- Transport implementations: `src/net/tcp_transport.cpp`, `src/net/connection_pool.cpp`
- Proxy implementation: `src/ref/actor_proxy.cpp`
- CLI commands: `src/cli/commands/`
- Metrics events: `include/hpactor/metrics/metrics_event.hpp`

## File Structure

Create:

- `include/hpactor/mailbox/delivery_result.hpp` — `DeliveryStatus` enum, `DeliveryResult` struct, constexpr mapping functions, `EnqueueResult::to_delivery_result()` method declaration
- `src/mailbox/delivery_result.cpp` — `DeliveryResult::from_enqueue()`, `DeliveryResult::from_transport()` implementations
- `tests/unit/mailbox/test_delivery_result.cpp` — unit tests for mapping, accessors, factories
- `tests/unit/net/test_transport_send_result.cpp` — unit tests for `TransportSendResult` mapping
- `tests/unit/actor/test_try_reply.cpp` — unit tests for `try_reply()`
- `tests/unit/core/test_deliver_with_result.cpp` — unit tests for `deliver_with_result()`
- `tests/integration/actor/test_delivery_result_integration.cpp` — end-to-end integration tests
- `tests/unit/cli/test_delivery_commands.cpp` — CLI delivery command tests

Modify:

- `include/hpactor/mailbox/mailbox_policy.hpp` — add `EnqueueResult::to_delivery_result()`, forward-declare `DeliveryResult`
- `include/hpactor/net/transport.hpp` — add `TransportSendResult` enum, change `try_send()` return type
- `include/hpactor/net/tcp_transport.hpp` — update `try_send()` return type
- `include/hpactor/net/connection_pool.hpp` — update `try_send()` return type
- `include/hpactor/net/endpoint_outbound_queue.hpp` — update `try_enqueue()` return type
- `include/hpactor/net/udp_transport.hpp` — update UDP transport `send()`/`try_send()` return types where applicable
- `include/hpactor/actor_context.hpp` — change `try_send()` + `try_send_with_priority()` return type; add `try_reply()`
- `include/hpactor/ref/actor_ref.hpp` — change `try_send()` return type
- `include/hpactor/ref/actor_proxy.hpp` — change `try_send()` return type
- `include/hpactor/core/actor_system.hpp` — add `deliver_with_result()`
- `src/actor/actor_context.cpp` — update `try_send()` impl; add `try_reply()` impl
- `src/actor/actor_system.cpp` — add `deliver_with_result()` impl
- `src/ref/actor_ref.cpp` — update `try_send()` impl
- `src/ref/actor_proxy.cpp` — map `TransportSendResult` → `DeliveryResult`
- `src/net/tcp_transport.cpp` — return `TransportSendResult`
- `src/net/connection_pool.cpp` — return `TransportSendResult`
- `src/net/endpoint_outbound_queue.cpp` — return `TransportSendResult`
- `src/net/udp_transport.cpp` — update return type
- `src/cli/commands/actor_commands.cpp` — add `/actor delivery` and `/actor delivery-stats` handlers
- `src/cli/commands/misc_commands.cpp` — register new delivery command group
- `include/hpactor/metrics/metrics_event.hpp` — add `kDeliveryResult` event type
- `src/metrics/metrics_aggregator.cpp` — handle `kDeliveryResult` event
- `src/CMakeLists.txt` — compile `src/mailbox/delivery_result.cpp`
- `tests/unit/mailbox/CMakeLists.txt` — add `test_delivery_result.cpp`
- `tests/unit/net/CMakeLists.txt` — add `test_transport_send_result.cpp`
- `tests/unit/actor/CMakeLists.txt` — add `test_try_reply.cpp`
- `tests/unit/core/CMakeLists.txt` — add `test_deliver_with_result.cpp`
- `tests/integration/actor/CMakeLists.txt` — add `test_delivery_result_integration.cpp`
- `tests/unit/cli/CMakeLists.txt` — add `test_delivery_commands.cpp`

## Targeted Verification Commands

If `build/` does not exist:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

For type-level unit tests:

```bash
cmake --build build --target test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="DeliveryResult*"
```

For transport result unit tests:

```bash
cmake --build build --target test_unit_net
./build/tests/unit/net/test_unit_net --gtest_filter="TransportSendResult*"
```

For actor API unit tests:

```bash
cmake --build build --target test_unit_actor
./build/tests/unit/actor/test_unit_actor --gtest_filter="TryReply*"
```

For core deliver_with_result tests:

```bash
cmake --build build --target test_unit_core
./build/tests/unit/core/test_unit_core --gtest_filter="DeliverWithResult*"
```

For CLI delivery command tests:

```bash
cmake --build build --target test_unit_cli
./build/tests/unit/cli/test_unit_cli --gtest_filter="DeliveryCommands*"
```

For integration tests:

```bash
cmake --build build --target test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter="DeliveryResult*"
```

For final targeted verification:

```bash
cmake --build build --target test_unit_mailbox test_unit_net test_unit_actor test_unit_core test_unit_cli test_integration_actor
ctest -R "DeliveryResult|TransportSendResult|TryReply|DeliverWithResult|DeliveryCommands" --output-on-failure
```

---

### Task 1: DeliveryStatus + DeliveryResult Types + EnqueueResult Mapping

**Note:** TDDFlow RED → GREEN → REFACTOR. Write the failing test first, then the implementation. Each step below describes the full loop.

**Files:**
- Create: `include/hpactor/mailbox/delivery_result.hpp`
- Create: `src/mailbox/delivery_result.cpp`
- Create: `tests/unit/mailbox/test_delivery_result.cpp`
- Modify: `include/hpactor/mailbox/mailbox_policy.hpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

- [ ] **Step 1a (RED): Write unit tests for DeliveryStatus + DeliveryResult + mapping**

Create `tests/unit/mailbox/test_delivery_result.cpp` with tests covering:

1. `DeliveryStatus` enum values (explicit values 0-11).
2. `to_string(DeliveryStatus)` for all 12 values.
3. `is_accepted(DeliveryStatus)` — true for Accepted, AcceptedWithPressure.
4. `is_retryable(DeliveryStatus)` — correct for each status.
5. `to_failure_reason(DeliveryStatus)` — maps each status to the canonical `FailureReason`.
6. `DeliveryResult` default construction — status=Accepted, empty target, zero message_id.
7. `DeliveryResult::ok()`, `.accepted()`, `.retryable()`, `.failure_reason()` accessors.
8. `DeliveryResult::from_enqueue()` — for all 11 `EnqueueResultCode` values, verify:
   - `Accepted` → `DeliveryStatus::Accepted`
   - `AcceptedWithSoftPressure` → `DeliveryStatus::AcceptedWithPressure`
   - `Rejected` → `DeliveryStatus::MailboxFull`
   - `DroppedNewest` / `DroppedExisting` → `DeliveryStatus::MailboxFull`
   - `ReroutedToDeadLetter` → `DeliveryStatus::RejectedByPolicy`
   - `ReroutedToOverflow` → `DeliveryStatus::AcceptedWithPressure`
   - `MailboxClosed` → `DeliveryStatus::ActorDead`
   - `ActorNotFound` → `DeliveryStatus::NoRoute`
   - `EndpointBackpressure` → `DeliveryStatus::RemoteUnavailable`
   - `EndpointCircuitOpen` → `DeliveryStatus::RemoteUnavailable`
9. `DeliveryResult::from_transport()` — for all 7 `TransportSendResult` values.
10. `EnqueueResult::to_delivery_result()` method — chains through `from_enqueue()`.

**Verification (RED):**

```bash
cmake --build build --target test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="DeliveryResult*"
# Expect: tests not found or compilation failure (types missing)
```

To get compilation far enough to see test failures rather than compile errors, first add stubs in the header (empty namespace, forward declares) so the test file compiles but all assertions fail.

- [ ] **Step 1b (GREEN): Implement the types**

Create `include/hpactor/mailbox/delivery_result.hpp`:

```cpp
#pragma once

#include <hpactor/mailbox/delivery_mode.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>  // For EnqueueResult, EnqueueResultCode
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor {

// Forward declaration for TransportSendResult (defined in net/transport.hpp)
enum class TransportSendResult : uint8_t;

namespace mailbox {

enum class DeliveryStatus : uint8_t {
    Accepted = 0,
    AcceptedWithPressure = 1,
    NoRoute = 2,
    ActorDead = 3,
    MailboxFull = 4,
    Expired = 5,
    Duplicate = 6,
    RemoteUnavailable = 7,
    RejectedByPolicy = 8,
    SerializationError = 9,
    TransportError = 10,
    ShuttingDown = 11,
};

constexpr bool is_accepted(DeliveryStatus s) noexcept {
    return s == DeliveryStatus::Accepted ||
           s == DeliveryStatus::AcceptedWithPressure;
}

constexpr bool is_retryable(DeliveryStatus s) noexcept {
    switch (s) {
        case DeliveryStatus::NoRoute:
        case DeliveryStatus::ActorDead:
        case DeliveryStatus::MailboxFull:
        case DeliveryStatus::RemoteUnavailable:
        case DeliveryStatus::TransportError:
            return true;
        default:
            return false;
    }
}

constexpr FailureReason to_failure_reason(DeliveryStatus s) noexcept;
const char* to_string(DeliveryStatus s) noexcept;

struct DeliveryResult {
    DeliveryStatus status{DeliveryStatus::Accepted};
    ActorAddress target;
    MessageId message_id{};
    uint32_t detail_code{0};

    [[nodiscard]] bool ok() const noexcept { return is_accepted(status); }
    [[nodiscard]] bool accepted() const noexcept { return is_accepted(status); }
    [[nodiscard]] bool retryable() const noexcept { return is_retryable(status); }
    [[nodiscard]] FailureReason failure_reason() const noexcept {
        return to_failure_reason(status);
    }

    static DeliveryResult from_enqueue(const EnqueueResult& er,
                                        const ActorAddress& target_addr,
                                        MessageId msg_id = {});
    static DeliveryResult from_transport(TransportSendResult tsr,
                                          const ActorAddress& target_addr,
                                          MessageId msg_id = {});
};

} // namespace mailbox
} // namespace hpactor

// Define TransportSendResult here to avoid circular includes between
// mailbox/delivery_result.hpp and net/transport.hpp.
// The canonical definition lives in net/transport.hpp; this forward
// declaration is sufficient for DeliveryResult::from_transport().
namespace hpactor {

enum class TransportSendResult : uint8_t {
    Sent = 0,
    NotConnected = 1,
    QueueFull = 2,
    CircuitOpen = 3,
    EncodeError = 4,
    ShuttingDown = 5,
    WriteError = 6,
};

} // namespace hpactor

namespace hpactor::mailbox {

// Now the constexpr mapping can reference the enum values
constexpr FailureReason to_failure_reason(DeliveryStatus s) noexcept {
    switch (s) {
        case DeliveryStatus::Accepted:
        case DeliveryStatus::AcceptedWithPressure:
            return FailureReason::Unknown;
        case DeliveryStatus::NoRoute:
            return FailureReason::NoRoute;
        case DeliveryStatus::ActorDead:
            return FailureReason::ActorTerminated;
        case DeliveryStatus::MailboxFull:
            return FailureReason::MailboxFull;
        case DeliveryStatus::Expired:
            return FailureReason::Timeout;
        case DeliveryStatus::Duplicate:
            return FailureReason::Deduplicated;
        case DeliveryStatus::RemoteUnavailable:
            return FailureReason::RemoteUnavailable;
        case DeliveryStatus::RejectedByPolicy:
            return FailureReason::RejectedByPolicy;
        case DeliveryStatus::SerializationError:
            return FailureReason::SerializationError;
        case DeliveryStatus::TransportError:
            return FailureReason::TransportError;
        case DeliveryStatus::ShuttingDown:
            return FailureReason::ShuttingDown;
    }
    return FailureReason::Unknown;
}

} // namespace hpactor::mailbox
```

Create `src/mailbox/delivery_result.cpp`:

```cpp
#include <hpactor/mailbox/delivery_result.hpp>

namespace hpactor::mailbox {

const char* to_string(DeliveryStatus s) noexcept {
    switch (s) {
        case DeliveryStatus::Accepted:           return "accepted";
        case DeliveryStatus::AcceptedWithPressure: return "accepted_with_pressure";
        case DeliveryStatus::NoRoute:            return "no_route";
        case DeliveryStatus::ActorDead:          return "actor_dead";
        case DeliveryStatus::MailboxFull:        return "mailbox_full";
        case DeliveryStatus::Expired:            return "expired";
        case DeliveryStatus::Duplicate:          return "duplicate";
        case DeliveryStatus::RemoteUnavailable:  return "remote_unavailable";
        case DeliveryStatus::RejectedByPolicy:   return "rejected_by_policy";
        case DeliveryStatus::SerializationError: return "serialization_error";
        case DeliveryStatus::TransportError:     return "transport_error";
        case DeliveryStatus::ShuttingDown:       return "shutting_down";
    }
    return "accepted";
}

DeliveryResult DeliveryResult::from_enqueue(const EnqueueResult& er,
                                              const ActorAddress& target_addr,
                                              MessageId msg_id) {
    DeliveryResult dr;
    dr.target = target_addr;
    dr.message_id = msg_id;

    switch (er.code) {
        case EnqueueResultCode::Accepted:
            dr.status = DeliveryStatus::Accepted;
            break;
        case EnqueueResultCode::AcceptedWithSoftPressure:
            dr.status = DeliveryStatus::AcceptedWithPressure;
            break;
        case EnqueueResultCode::Rejected:
            dr.status = DeliveryStatus::MailboxFull;
            break;
        case EnqueueResultCode::DroppedNewest:
        case EnqueueResultCode::DroppedExisting:
            dr.status = DeliveryStatus::MailboxFull;
            break;
        case EnqueueResultCode::ReroutedToDeadLetter:
            dr.status = DeliveryStatus::RejectedByPolicy;
            break;
        case EnqueueResultCode::ReroutedToOverflow:
            dr.status = DeliveryStatus::AcceptedWithPressure;
            break;
        case EnqueueResultCode::MailboxClosed:
            dr.status = DeliveryStatus::ActorDead;
            break;
        case EnqueueResultCode::ActorNotFound:
            dr.status = DeliveryStatus::NoRoute;
            break;
        case EnqueueResultCode::EndpointBackpressure:
            dr.status = DeliveryStatus::RemoteUnavailable;
            break;
        case EnqueueResultCode::EndpointCircuitOpen:
            dr.status = DeliveryStatus::RemoteUnavailable;
            break;
    }

    dr.detail_code = static_cast<uint32_t>(er.code);
    return dr;
}

DeliveryResult DeliveryResult::from_transport(TransportSendResult tsr,
                                                const ActorAddress& target_addr,
                                                MessageId msg_id) {
    DeliveryResult dr;
    dr.target = target_addr;
    dr.message_id = msg_id;

    switch (tsr) {
        case TransportSendResult::Sent:
            dr.status = DeliveryStatus::Accepted;
            break;
        case TransportSendResult::NotConnected:
        case TransportSendResult::QueueFull:
        case TransportSendResult::CircuitOpen:
            dr.status = DeliveryStatus::RemoteUnavailable;
            break;
        case TransportSendResult::EncodeError:
            dr.status = DeliveryStatus::SerializationError;
            break;
        case TransportSendResult::ShuttingDown:
            dr.status = DeliveryStatus::ShuttingDown;
            break;
        case TransportSendResult::WriteError:
            dr.status = DeliveryStatus::TransportError;
            break;
    }

    dr.detail_code = static_cast<uint32_t>(tsr);
    return dr;
}

} // namespace hpactor::mailbox
```

In `include/hpactor/mailbox/mailbox_policy.hpp`, add to the `EnqueueResult` struct (after `failure_reason()`):

```cpp
    /// Convert to the user-facing DeliveryResult type.
    ///
    /// \param[in] target_addr Target actor address for the result.
    /// \param[in] msg_id Optional message id for correlation.
    /// \return DeliveryResult with status mapped from this code.
    /// \note Requires `delivery_result.hpp` to be included for the
    ///       return type to be complete.
    DeliveryResult to_delivery_result(const ActorAddress& target_addr,
                                       MessageId msg_id = {}) const;
```

Update `src/CMakeLists.txt` to add `src/mailbox/delivery_result.cpp` to the hpactor_lib sources.

Update `tests/unit/mailbox/CMakeLists.txt` to add `test_delivery_result.cpp`.

**Verification (GREEN):**

```bash
cmake --build build --target test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="DeliveryResult*"
# Expect: all tests pass
```

- [ ] **Step 1c (REFACTOR): Clean up**

- Verify no unnecessary includes in `delivery_result.hpp`.
- Verify all `to_string()` strings follow the snake_case convention used by other mailbox types.
- Verify the `TransportSendResult` forward declaration comment is accurate.

**Verification after refactor:**

```bash
./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="DeliveryResult*"
# Expect: still all green
```

---

### Task 2: TransportSendResult — Transport Layer Return Type Change

**Files:**
- Modify: `include/hpactor/net/transport.hpp`
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `include/hpactor/net/endpoint_outbound_queue.hpp`
- Modify: `include/hpactor/net/udp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`
- Modify: `src/net/connection_pool.cpp`
- Modify: `src/net/endpoint_outbound_queue.cpp`
- Modify: `src/net/udp_transport.cpp`
- Create: `tests/unit/net/test_transport_send_result.cpp`
- Modify: `tests/unit/net/CMakeLists.txt`

- [ ] **Step 2a (RED): Write transport send result tests**

Create `tests/unit/net/test_transport_send_result.cpp`:

```cpp
#include <hpactor/mailbox/delivery_result.hpp>

#include <gtest/gtest.h>

TEST(TransportSendResultTest, EnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(TransportSendResult::Sent), 0);
    EXPECT_EQ(static_cast<uint8_t>(TransportSendResult::NotConnected), 1);
    EXPECT_EQ(static_cast<uint8_t>(TransportSendResult::QueueFull), 2);
    EXPECT_EQ(static_cast<uint8_t>(TransportSendResult::CircuitOpen), 3);
    EXPECT_EQ(static_cast<uint8_t>(TransportSendResult::EncodeError), 4);
    EXPECT_EQ(static_cast<uint8_t>(TransportSendResult::ShuttingDown), 5);
    EXPECT_EQ(static_cast<uint8_t>(TransportSendResult::WriteError), 6);
}

TEST(TransportSendResultTest, ToDeliveryResultSent) {
    auto dr = mailbox::DeliveryResult::from_transport(
        TransportSendResult::Sent, ActorAddress{}, MessageId{1});
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::Accepted);
    EXPECT_TRUE(dr.ok());
}

TEST(TransportSendResultTest, ToDeliveryResultNotConnected) {
    auto dr = mailbox::DeliveryResult::from_transport(
        TransportSendResult::NotConnected, ActorAddress{}, MessageId{});
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
}

TEST(TransportSendResultTest, ToDeliveryResultQueueFull) {
    auto dr = mailbox::DeliveryResult::from_transport(
        TransportSendResult::QueueFull, ActorAddress{}, MessageId{});
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
}

TEST(TransportSendResultTest, ToDeliveryResultCircuitOpen) {
    auto dr = mailbox::DeliveryResult::from_transport(
        TransportSendResult::CircuitOpen, ActorAddress{}, MessageId{});
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::RemoteUnavailable);
    EXPECT_FALSE(dr.retryable());
}

TEST(TransportSendResultTest, ToDeliveryResultEncodeError) {
    auto dr = mailbox::DeliveryResult::from_transport(
        TransportSendResult::EncodeError, ActorAddress{}, MessageId{});
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::SerializationError);
    EXPECT_FALSE(dr.retryable());
}

TEST(TransportSendResultTest, ToDeliveryResultShuttingDown) {
    auto dr = mailbox::DeliveryResult::from_transport(
        TransportSendResult::ShuttingDown, ActorAddress{}, MessageId{});
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::ShuttingDown);
    EXPECT_FALSE(dr.retryable());
}

TEST(TransportSendResultTest, ToDeliveryResultWriteError) {
    auto dr = mailbox::DeliveryResult::from_transport(
        TransportSendResult::WriteError, ActorAddress{}, MessageId{});
    EXPECT_EQ(dr.status, mailbox::DeliveryStatus::TransportError);
    EXPECT_TRUE(dr.retryable());
}
```

Update `tests/unit/net/CMakeLists.txt` to add `test_transport_send_result.cpp`.

**Verification (RED):** Should compile but the mapping tests may fail until the transport implementations actually return `TransportSendResult`. However, since `DeliveryResult::from_transport()` already exists from Task 1, the mapping tests should pass. The RED step here validates that the transport implementations still compile after the signature change — but they won't compile because the signatures still return `bool`.

```bash
cmake --build build --target test_unit_net
# Expect: compilation errors in transport impls (still return bool)
```

- [ ] **Step 2b (GREEN): Change transport signatures and implementations**

In `include/hpactor/net/transport.hpp`, change:

```cpp
// Before:
virtual bool try_send(const ActorAddress& target,
                      const StreamBuffer& encoded) = 0;

// After:
virtual TransportSendResult try_send(const ActorAddress& target,
                                      const StreamBuffer& encoded) = 0;
```

Update the `send()` default implementation to discard `TransportSendResult`:

```cpp
virtual void send(const ActorAddress& target, const StreamBuffer& encoded) {
    (void)try_send(target, encoded);
}
```

In `include/hpactor/net/tcp_transport.hpp`:

```cpp
// Before:
bool try_send(const ActorAddress& target,
              const StreamBuffer& encoded) override;

// After:
TransportSendResult try_send(const ActorAddress& target,
                               const StreamBuffer& encoded) override;
```

In `include/hpactor/net/connection_pool.hpp`:

```cpp
// Before:
bool try_send(const ActorAddress& target, const StreamBuffer& encoded);

// After:
TransportSendResult try_send(const ActorAddress& target,
                               const StreamBuffer& encoded);
```

In `include/hpactor/net/endpoint_outbound_queue.hpp`:

```cpp
// Before:
mailbox::EnqueueResult try_enqueue(PendingMessage msg,
                                    mailbox::DeliveryMode mode,
                                    TypeTag type_tag);

// After:
TransportSendResult try_enqueue(PendingMessage msg,
                                  mailbox::DeliveryMode mode,
                                  TypeTag type_tag);
```

In `src/net/tcp_transport.cpp`, update `TcpTransport::try_send()`:

```cpp
TransportSendResult TcpTransport::try_send(const ActorAddress& target,
                                            const StreamBuffer& encoded) {
    if (!pool_) {
        return TransportSendResult::NotConnected;
    }
    return pool_->try_send(target, encoded);
}
```

In `src/net/connection_pool.cpp`, update `ConnectionPool::try_send()` to return `TransportSendResult`:

```cpp
TransportSendResult
ConnectionPool::try_send(const ActorAddress& target,
                           const StreamBuffer& encoded) {
    // ... existing logic mapping failure paths to TransportSendResult values:
    // - no connection found → TransportSendResult::NotConnected
    // - queue full → TransportSendResult::QueueFull
    // - circuit breaker open → TransportSendResult::CircuitOpen
    // - write error → TransportSendResult::WriteError
    // - shutting down → TransportSendResult::ShuttingDown
    // - successfully queued → TransportSendResult::Sent
}
```

Update `src/net/endpoint_outbound_queue.cpp` similarly.

For `src/net/udp_transport.cpp`, update to return `TransportSendResult::Sent` on success and `TransportSendResult::WriteError` on failure.

**Verification (GREEN):**

```bash
cmake --build build --target test_unit_net
./build/tests/unit/net/test_unit_net --gtest_filter="TransportSendResult*"
# Expect: all tests pass, transport impls compile
```

- [ ] **Step 2c (REFACTOR):** Verify all transport `try_send()` implementations return the most specific `TransportSendResult` for each failure path. No `NotConnected` where `CircuitOpen` is more precise.

---

### Task 3: ActorProxy::try_send() → DeliveryResult

**Files:**
- Modify: `include/hpactor/ref/actor_proxy.hpp`
- Modify: `src/ref/actor_proxy.cpp`

- [ ] **Step 3a (RED): The proxy already has tests in `test_actor_proxy.cpp`. Update the test expectations to check for `DeliveryResult` instead of `EnqueueResult`. Build and confirm test failures.**

```bash
cmake --build build --target test_unit_ref
./build/tests/unit/ref/test_unit_ref --gtest_filter="ActorProxy*"
# Expect: compilation error or assertion failures due to type mismatch
```

- [ ] **Step 3b (GREEN): Update ActorProxy**

In `include/hpactor/ref/actor_proxy.hpp`:

```cpp
// Before:
mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                 mailbox::DeliveryOptions options = {});

// After:
mailbox::DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                                   mailbox::DeliveryOptions options = {});
```

Add the include for `delivery_result.hpp` at the top.

In `src/ref/actor_proxy.cpp`, update `try_send()`:

```cpp
mailbox::DeliveryResult
ActorProxy::try_send(const ActorAddress& target, TypedMessage msg,
                      mailbox::DeliveryOptions /*options*/) {
    if (transport_ == nullptr) {
        // DLQ capture (existing code unchanged)
        // ...
        return mailbox::DeliveryResult{
            mailbox::DeliveryStatus::NoRoute, target, {}, 0};
    }

    // ... route resolution (existing code unchanged) ...

    TransportSendResult tsr = transport_->try_send(resolved_target,
                                                     frame.encode());
    if (tsr != TransportSendResult::Sent) {
        // DLQ capture (existing code unchanged, but use tsr for reason selection)
        // ...
    }
    return mailbox::DeliveryResult::from_transport(tsr, target,
                                                     MessageId{frame.pb_frame.message_id()});
}
```

Also update `ActorProxy::send()` which currently calls `try_send()` and discards the result — this path needs no change since `DeliveryResult` is discardable.

**Verification (GREEN):**

```bash
cmake --build build --target test_unit_ref
./build/tests/unit/ref/test_unit_ref --gtest_filter="ActorProxy*"
# Expect: all tests pass
```

---

### Task 4: ActorContext::try_send() + try_reply() → DeliveryResult

**Files:**
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Create: `tests/unit/actor/test_try_reply.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

- [ ] **Step 4a (RED): Write try_reply tests + update actor_context tests**

Create `tests/unit/actor/test_try_reply.cpp` with tests for:
1. `try_reply()` with valid sender — returns `Accepted`.
2. `try_reply()` with no current sender — returns `NoRoute`.
3. `try_reply()` passes `DeliveryOptions` through correctly.

Update existing `test_actor_context.cpp` tests that call `try_send()` to expect `DeliveryResult` instead of `EnqueueResult`.

**Verification (RED):**

```bash
cmake --build build --target test_unit_actor
./build/tests/unit/actor/test_unit_actor --gtest_filter="TryReply*"
# Expect: test failures (method doesn't exist yet)
```

- [ ] **Step 4b (GREEN): Update ActorContext**

In `include/hpactor/actor_context.hpp`:

```cpp
// Before:
mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                 mailbox::DeliveryOptions options = {});

mailbox::EnqueueResult try_send_with_priority(const ActorAddress& target,
                                               TypedMessage msg,
                                               uint8_t priority,
                                               int64_t deadline_ns,
                                               mailbox::DeliveryOptions options = {});

// After:
mailbox::DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                                   mailbox::DeliveryOptions options = {});

mailbox::DeliveryResult try_send_with_priority(const ActorAddress& target,
                                                 TypedMessage msg,
                                                 uint8_t priority,
                                                 int64_t deadline_ns,
                                                 mailbox::DeliveryOptions options = {});

// New:
mailbox::DeliveryResult try_reply(TypedMessage msg,
                                    mailbox::DeliveryOptions options = {});
```

In `src/actor/actor_context.cpp`, update `try_send()` to convert the internal `EnqueueResult` → `DeliveryResult` via `DeliveryResult::from_enqueue()`. The internal path through `ActorRef::try_send()` → `ActorSystem::try_deliver_local()` still produces `EnqueueResult`; the conversion happens at the `ActorContext` boundary.

Add `try_reply()` implementation:

```cpp
mailbox::DeliveryResult
ActorContext::try_reply(TypedMessage msg, mailbox::DeliveryOptions options) {
    if (!current_sender_) {
        return mailbox::DeliveryResult{
            mailbox::DeliveryStatus::NoRoute, ActorAddress{}, {}, 0};
    }
    return try_send(*current_sender_, std::move(msg), options);
}
```

**Verification (GREEN):**

```bash
cmake --build build --target test_unit_actor
./build/tests/unit/actor/test_unit_actor --gtest_filter="TryReply*"
# Expect: all tests pass
```

---

### Task 5: ActorRef::try_send() → DeliveryResult

**Files:**
- Modify: `include/hpactor/ref/actor_ref.hpp`
- Modify: `src/ref/actor_ref.cpp`

- [ ] **Step 5a (RED): Update test_actor_ref.cpp tests to expect DeliveryResult. Build and confirm compilation failures.**

- [ ] **Step 5b (GREEN): Update ActorRef**

In `include/hpactor/ref/actor_ref.hpp`:

```cpp
// Before:
mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                 mailbox::DeliveryOptions options = {});

// After:
mailbox::DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                                   mailbox::DeliveryOptions options = {});
```

In `src/ref/actor_ref.cpp`, delegate to the internal path and convert the result:

```cpp
mailbox::DeliveryResult
ActorRef::try_send(const ActorAddress& target, TypedMessage msg,
                    mailbox::DeliveryOptions options) {
    if (auto* actor = get_actor()) {
        auto* system = actor->system();
        if (system == nullptr) {
            return {mailbox::DeliveryStatus::NoRoute, target, {}, 0};
        }
        auto er = system->try_deliver_local(target.id, std::move(msg),
                                              0, INT64_MAX, options);
        return mailbox::DeliveryResult::from_enqueue(er, target,
                                                       MessageId{options.message_id});
    }
    if (auto* proxy = get_proxy()) {
        return proxy->try_send(target, std::move(msg), options);
    }
    return {mailbox::DeliveryStatus::NoRoute, target, {}, 0};
}
```

Update `ActorRef::send()` — it currently discards `EnqueueResult`, now discards `DeliveryResult`. No behavioral change.

**Verification (GREEN):**

```bash
cmake --build build --target test_unit_ref
./build/tests/unit/ref/test_unit_ref --gtest_filter="ActorRef*"
# Expect: all tests pass
```

---

### Task 6: ActorSystem::deliver_with_result()

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Create: `tests/unit/core/test_deliver_with_result.cpp`
- Modify: `tests/unit/core/CMakeLists.txt`

- [ ] **Step 6a (RED): Write tests for deliver_with_result()**

Create `tests/unit/core/test_deliver_with_result.cpp`:
1. Deliver to existing actor → `DeliveryStatus::Accepted`.
2. Deliver to non-existent actor → `DeliveryStatus::NoRoute`.
3. Deliver to actor with full mailbox → `DeliveryStatus::MailboxFull`.
4. Verify `deliver_with_result()` passes through priority and deadline.

**Verification (RED):**

```bash
cmake --build build --target test_unit_core
./build/tests/unit/core/test_unit_core --gtest_filter="DeliverWithResult*"
# Expect: compilation failure (method doesn't exist)
```

- [ ] **Step 6b (GREEN): Add deliver_with_result()**

In `include/hpactor/core/actor_system.hpp`:

```cpp
/// \brief Deliver with a user-facing DeliveryResult.
///
/// Wraps \c try_deliver_local() and converts the internal
/// \c EnqueueResult to \c DeliveryResult.
mailbox::DeliveryResult deliver_with_result(ActorId target, TypedMessage msg,
                                              uint8_t priority = 0,
                                              int64_t deadline_ns = INT64_MAX,
                                              mailbox::DeliveryOptions options = {});
```

In `src/actor/actor_system.cpp`:

```cpp
mailbox::DeliveryResult
ActorSystem::deliver_with_result(ActorId target, TypedMessage msg,
                                  uint8_t priority, int64_t deadline_ns,
                                  mailbox::DeliveryOptions options) {
    auto er = try_deliver_local(target, std::move(msg), priority,
                                 deadline_ns, options);
    return mailbox::DeliveryResult::from_enqueue(er, ActorAddress{}, {});
}
```

**Verification (GREEN):**

```bash
cmake --build build --target test_unit_core
./build/tests/unit/core/test_unit_core --gtest_filter="DeliverWithResult*"
# Expect: all tests pass
```

---

### Task 7: Metrics Event + CLI Commands

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`
- Modify: `src/metrics/metrics_aggregator.cpp`
- Modify: `src/cli/commands/actor_commands.cpp` (or new file for delivery commands)
- Create: `tests/unit/cli/test_delivery_commands.cpp`
- Modify: `tests/unit/cli/CMakeLists.txt`

- [ ] **Step 7a (RED): Write CLI delivery command tests**

Create `tests/unit/cli/test_delivery_commands.cpp`:
1. `/actor delivery <id>` — shows recent delivery results for an actor.
2. `/actor delivery-stats <id>` — shows aggregated counts by status.
3. Edge cases: no deliveries yet, invalid actor id, actor not found.

**Verification (RED):**

```bash
cmake --build build --target test_unit_cli
./build/tests/unit/cli/test_unit_cli --gtest_filter="DeliveryCommands*"
# Expect: test failures (commands don't exist)
```

- [ ] **Step 7b (GREEN): Implement metrics event + CLI commands**

In `include/hpactor/metrics/metrics_event.hpp`, add:

```cpp
kDeliveryResult = 24,  // Delivery outcome event
```

In `src/metrics/metrics_aggregator.cpp`, handle `kDeliveryResult` by incrementing the `hpactor_delivery_results_total` counter with the `status` label.

In `src/cli/commands/actor_commands.cpp`, add delivery command handlers using the `CommandRegistration<T>` self-registration pattern:

```cpp
// /actor delivery <actor_id> — show recent delivery results
class ActorDeliveryCommand : public ICommand {
    // ...
    std::string execute(const CommandContext& ctx) override;
    static constexpr const char* kPath = "/actor";
    static constexpr const char* kSubPath = "delivery";
};

// /actor delivery-stats <actor_id> — aggregated counts
class ActorDeliveryStatsCommand : public ICommand {
    // ...
};
```

**Note:** Storage for recent delivery results per-actor is out of scope for this task. The CLI commands expose results from the existing `EnqueueResult` path via `MailboxSnapshot`. If the mailbox snapshot doesn't carry enough detail, add a small ring buffer of recent `DeliveryResult` values to `MPSCActorMailbox` (behind `ENABLE_ACTOR_METRICS`).

**Verification (GREEN):**

```bash
cmake --build build --target test_unit_cli
./build/tests/unit/cli/test_unit_cli --gtest_filter="DeliveryCommands*"
# Expect: all tests pass
```

---

### Task 8: Update Existing Tests for DeliveryResult Return Type

**Files:**
- Modify: `tests/unit/actor/test_actor_context.cpp`
- Modify: `tests/unit/ref/test_actor_ref.cpp`
- Modify: `tests/unit/ref/test_actor_proxy.cpp`
- Modify: `tests/unit/mailbox/test_delivery_mode.cpp` (if affected)
- Modify: `tests/unit/mailbox/test_dedup_cache.cpp` (if affected)
- Modify: Any integration test that calls `try_send()` and inspects `EnqueueResult`

- [ ] **Step 8a: Build everything and identify failures**

```bash
cmake --build build
# Collect all compilation errors in tests referencing EnqueueResult from try_send()
```

- [ ] **Step 8b: Update each test file**

For each test that calls `try_send()` and destructures the result:
- Replace `EnqueueResult` with `DeliveryResult` for the variable type.
- Replace `.code == EnqueueResultCode::Accepted` with `.status == DeliveryStatus::Accepted`.
- Replace `.accepted()` with `.accepted()` (unchanged method name).
- Replace `.retryable()` with `.retryable()` (unchanged method name).
- Replace direct field access (`.depth`, `.pressure_ratio`) with `.status` — if the test genuinely needs pressure detail, switch it to call `try_deliver_local()` directly.

Tests that call `try_deliver_local()` directly are NOT affected since its return type is unchanged.

**Verification:**

```bash
cmake --build build
ctest --output-on-failure --parallel 8
# Expect: all 32 GTest binaries pass, ~1411+new tests green
```

---

### Task 9: Integration Tests

**Files:**
- Create: `tests/integration/actor/test_delivery_result_integration.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

- [ ] **Step 9a (RED): Write integration tests**

Create `tests/integration/actor/test_delivery_result_integration.cpp`:
1. **Local delivery round-trip**: Spawn an actor, `try_send()` a message, verify `Accepted`.
2. **Local NoRoute**: `try_send()` to an actor id that was never spawned → `NoRoute`.
3. **Local MailboxFull**: Spawn an actor with capacity=1, fill the mailbox, `try_send()` → `MailboxFull`.
4. **Local Expired**: `try_send()` with a past deadline → `Expired`.
5. **Remote Accepted**: `try_send()` to a remote actor (loopback transport) → `Accepted`.
6. **Remote NotConnected**: `try_send()` with no transport available → `NoRoute` or `RemoteUnavailable`.
7. **try_reply() round-trip**: Actor receives a message, calls `try_reply()` → `Accepted`.
8. **deliver_with_result()**: Call from main thread → `Accepted`.

**Verification (RED):**

```bash
cmake --build build --target test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter="DeliveryResult*"
# Expect: test failures (tests don't exist yet)
```

- [ ] **Step 9b (GREEN): Implement integration tests**

Write the test cases. Tests requiring scheduler dispatch use condition-based polling with 5s timeout (per test design constraints in CLAUDE.md). Tests that don't need the scheduler use `scheduler_threads = 0`.

**Verification (GREEN):**

```bash
cmake --build build --target test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter="DeliveryResult*"
# Expect: all integration tests pass
```

---

### Task 10: Final Verification

- [ ] **Step 10a: Full build**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
# Expect: zero warnings, zero errors
```

- [ ] **Step 10b: Full test suite**

```bash
ctest --output-on-failure --parallel 8
# Expect: all tests pass, no regressions
```

- [ ] **Step 10c: Sanitizer pass**

```bash
cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build-asan
ctest --test-dir build-asan --output-on-failure --parallel 8
# Expect: no new ASAN failures (baseline exclusions: test_mailbox_awaiter, test_priority_scheduler)
```

- [ ] **Step 10d: Verify git diff is clean and coherent**

```bash
git diff --check
git diff --stat
```

- [ ] **Step 10e: Verify all new files have Apache 2.0 license headers**

```bash
grep -L "Apache License" docs/superpowers/specs/2026-06-04-msg002-result-delivery-api-design.md \
  include/hpactor/mailbox/delivery_result.hpp \
  src/mailbox/delivery_result.cpp \
  tests/unit/mailbox/test_delivery_result.cpp \
  tests/unit/net/test_transport_send_result.cpp \
  tests/unit/actor/test_try_reply.cpp \
  tests/unit/core/test_deliver_with_result.cpp \
  tests/integration/actor/test_delivery_result_integration.cpp \
  tests/unit/cli/test_delivery_commands.cpp
# Expect: empty output (all files have license headers)
```

---

## Task Dependency Graph

```
Task 1 (types + mapping)
 ├── Task 2 (transport result) ──┐
 ├── Task 4 (ActorContext)       │
 ├── Task 5 (ActorRef)           │
 ├── Task 6 (ActorSystem)        │
 └── Task 3 (ActorProxy) ←───────┘ (depends on Task 2)
      │
      └── Task 7 (metrics + CLI)
           │
           └── Task 8 (update existing tests)
                │
                └── Task 9 (integration tests)
                     │
                     └── Task 10 (final verification)
```

Tasks 2, 4, 5, and 6 can run in parallel after Task 1 completes.
Tasks 7-10 are sequential.
