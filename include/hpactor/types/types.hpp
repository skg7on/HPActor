// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorId - unique identifier for an actor instance
// -----------------------------------------------------------------------------
struct ActorId {
    using counter_type = uint64_t;

    ActorId() = default;

    explicit constexpr ActorId(counter_type value) : value_(value) {}

    counter_type value() const noexcept {
        return value_;
    }

    bool operator==(const ActorId& other) const noexcept {
        return value_ == other.value_;
    }
    bool operator!=(const ActorId& other) const noexcept {
        return !(*this == other);
    }

  private:
    counter_type value_ = 0;
};

// -----------------------------------------------------------------------------
// NodeId - identifier for a node in a distributed system
// 0 = local node, > 0 = remote node
// -----------------------------------------------------------------------------
using NodeId = uint32_t;
constexpr NodeId LocalNodeId = 0;

// -----------------------------------------------------------------------------
// ActorType - type identifier for an actor
// -----------------------------------------------------------------------------
using ActorType = uint32_t;
constexpr ActorType InvalidActorType = 0;

// -----------------------------------------------------------------------------
// incarnation_type - version counter for actor lifecycle
// -----------------------------------------------------------------------------
using incarnation_type = uint64_t;

// -----------------------------------------------------------------------------
// MessageId - unique identifier for a message
// -----------------------------------------------------------------------------
struct MessageId {
    using counter_type = uint64_t;

    MessageId() = default;

    explicit MessageId(counter_type value) : value_(value) {}

    counter_type value() const {
        return value_;
    }

    bool operator==(const MessageId& other) const {
        return value_ == other.value_;
    }
    bool operator!=(const MessageId& other) const {
        return !(*this == other);
    }

    static MessageId generate() {
        return MessageId(next_id_.fetch_add(1));
    }

  private:
    counter_type value_ = 0;
    static std::atomic<uint64_t> next_id_;
};

// Definition of static member
inline std::atomic<uint64_t> MessageId::next_id_{1};

// -----------------------------------------------------------------------------
// error - error code wrapper (no exceptions in hot path)
// -----------------------------------------------------------------------------
class error {
  public:
    error() = default;

    explicit error(uint32_t code, std::string msg = {})
        : code_(code), message_(std::move(msg)) {}

    uint32_t code() const {
        return code_;
    }
    const std::string& message() const {
        return message_;
    }

    bool ok() const {
        return code_ == 0;
    }
    explicit operator bool() const {
        return !ok();
    }

  private:
    uint32_t code_ = 0;
    std::string message_;
};

// -----------------------------------------------------------------------------
// errors namespace - canonical error codes
// -----------------------------------------------------------------------------
namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;
constexpr uint32_t user = 1000;
} // namespace errors

// -----------------------------------------------------------------------------
// result<T> - return type for message handlers
// -----------------------------------------------------------------------------
template <typename T> class result {
  public:
    static result<T> make(T&& value) {
        return result<T>(std::move(value));
    }
    static result<T> make(class error err) {
        return result<T>(std::move(err));
    }

    bool has_value() const {
        return has_value_;
    }
    T& value() {
        return std::get<0>(value_);
    }
    const class error& error() const {
        return std::get<1>(value_);
    }

  private:
    result(T&& val) : has_value_(true), value_(std::move(val)) {}
    result(class error err) : has_value_(false), value_(err) {}

    bool has_value_;
    std::variant<T, class error> value_;
};

template <> class result<void> {
  public:
    static result<void> make() {
        return result<void>();
    }
    static result<void> make(class error err) {
        return result<void>(std::move(err));
    }

    bool has_value() const {
        return has_value_;
    }
    void value() const {} // No-op for void
    const class error& error() const {
        return error_;
    }

  private:
    result<void>() : has_value_(true) {}
    result<void>(class error err) : has_value_(false), error_(std::move(err)) {}

    bool has_value_;
    class error error_;
};

// -----------------------------------------------------------------------------
// Clock - for time-based operations
// -----------------------------------------------------------------------------
class Clock {
  public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::milliseconds;
    time_point now() const {
        return std::chrono::steady_clock::now();
    }
};

// -----------------------------------------------------------------------------
// AlarmHandle - opaque handle for alarms
// -----------------------------------------------------------------------------
struct AlarmHandle {
    AlarmHandle() = default;

    explicit AlarmHandle(uint64_t id) : id_(id) {}

    uint64_t id() const {
        return id_;
    }

  private:
    uint64_t id_ = 0;
};

// -----------------------------------------------------------------------------
// TraceContext - for distributed tracing
// -----------------------------------------------------------------------------
struct TraceContext {
    TraceContext() = default;

    TraceContext(uint64_t trace_id, uint64_t span_id, uint32_t flags)
        : trace_id_(trace_id), span_id_(span_id), flags_(flags) {}

    uint64_t trace_id() const {
        return trace_id_;
    }
    uint64_t span_id() const {
        return span_id_;
    }
    uint32_t flags() const {
        return flags_;
    }

  private:
    uint64_t trace_id_ = 0;
    uint64_t span_id_ = 0;
    uint32_t flags_ = 0;
};

// -----------------------------------------------------------------------------
// bytes - byte buffer type
// -----------------------------------------------------------------------------
using bytes = std::vector<uint8_t>;

// -----------------------------------------------------------------------------
// TypeTag - type identifier for serialization (replaces RTTI)
// Each serializable type gets a unique tag. System messages use tags 0-99,
// user messages use tags 100+.
// -----------------------------------------------------------------------------
enum class TypeTag : uint32_t {
    Invalid = 0,

    // System messages (always present)
    DownMsg = 1,
    ExitMsg = 2,
    LinkMsg = 3,
    UnlinkMsg = 4,

    // First available user tag
    User = 100,
};

} // namespace hpactor
