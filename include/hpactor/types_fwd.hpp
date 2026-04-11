#pragma once

#include <cstdint>

namespace hpactor {

// Forward declarations

struct ActorId;
struct MessageId;
struct TraceContext;
class error;

using NodeId = uint32_t;
using ActorType = uint32_t;
using incarnation_type = uint64_t;
using AlarmHandle = uint64_t;
using Clock = std::chrono::steady_clock;

namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;
constexpr uint32_t user = 1000;
} // namespace errors

constexpr NodeId InvalidNodeId = 0;
constexpr ActorType InvalidActorType = 0;

using bytes = std::vector<uint8_t>;

} // namespace hpactor