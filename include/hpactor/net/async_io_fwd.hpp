// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/types/types.hpp>
#include <sys/socket.h>
#include <sys/uio.h>

#include <functional>

namespace hpactor {
namespace net {

enum class OpType : uint32_t {
    Send = 1,
    Recv = 2,
    Accept = 3,
    Connect = 4,
    TimerFired = 5,
    RecvFrom = 6,
    SendTo = 7,
};

struct OpCompletion {
    ActorId actor;
    OpType type;
    int fd;
    int result;
    uint64_t user_data = 0;
    sockaddr_in src_addr = {};
    socklen_t src_addr_len = 0;
};

// Read handler callback type - called when fd is readable.
// The callback owns the read: it must ::read() from the fd directly.
using read_callback = std::function<void(int fd)>;

// Write handler callback type - called when fd is writable.
// Used for non-blocking connect completion: callback checks SO_ERROR
// and transitions the connection to Connected on success.
using write_callback = std::function<void(int fd)>;

// Encode actor, fd, and op_type into a single uint64_t for user_data field.
// Format: bits 0-31 = fd, bits 32-47 = actor_id, bits 48-55 = reserved,
// bits 56-63 = op_type
inline uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

// Decode user_data back into fd, actor, and op_type
inline void decode_user_data(uint64_t user_data, int& fd, ActorId& actor,
                              uint32_t& op_type) {
    fd = static_cast<int>(user_data & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((user_data >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((user_data >> 56) & 0xFFULL);
}

} // namespace net
} // namespace hpactor