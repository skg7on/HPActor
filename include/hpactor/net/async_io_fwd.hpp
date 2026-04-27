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

// Read handler callback type - called when data is received
using read_callback = std::function<void(const bytes&)>;

} // namespace net
} // namespace hpactor