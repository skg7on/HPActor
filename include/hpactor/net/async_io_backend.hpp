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

#include <hpactor/types/types.hpp>
#include <sys/socket.h>
#include <sys/uio.h>

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

enum class IoEvent : uint32_t {
    Read = 1 << 0,
    Write = 1 << 1,
};

struct OpCompletion {
    ActorId actor;
    OpType type;
    int fd;             // fd the operation was on
    int result;         // >= 0 bytes on success, < 0 errno on failure
    uint64_t user_data; // original user_data from the SQE

    // For recvfrom: source address of received datagram
    struct sockaddr_in src_addr = {};
    socklen_t src_addr_len = 0;
};

class AsyncIoBackend {
  public:
    virtual ~AsyncIoBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual bool add_fd(int fd, IoEvent events) = 0;
    virtual bool update_fd(int fd, IoEvent events) = 0;
    virtual bool remove_fd(int fd) = 0;

    virtual int register_buffer(const void* addr, size_t len) = 0;
    virtual bool unregister_buffer(int buffer_id) = 0;

    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;

    virtual void async_send_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;

    virtual void async_accept(int fd, ActorId actor) = 0;
    virtual void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) = 0;

    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) = 0;
    virtual void
    async_sendto(int fd, const iovec* bufs, int buf_count, const sockaddr* addr,
                 socklen_t addrlen, ActorId actor, uint32_t op_type) = 0;

    virtual uint64_t run_after(ActorId actor, int delay_ms) = 0;
    virtual uint64_t run_every(ActorId actor, int interval_ms) = 0;
    virtual void cancel_timer(uint64_t handle) = 0;

    virtual int wait(int timeout_ms) = 0;
    virtual void process_completions() = 0;

    // Called by backend implementations to deliver a completion
    virtual void deliver_completion(OpCompletion completion) = 0;
};

[[maybe_unused]] static uint64_t
encode_user_data(int fd, ActorId actor, uint32_t op_type) {
    return (static_cast<uint64_t>(fd) & 0xFFFFFFFFULL) |
           ((actor.value() & 0xFFFFULL) << 32) |
           ((static_cast<uint64_t>(op_type) & 0xFFULL) << 56);
}

[[maybe_unused]] static void
decode_user_data(uint64_t ud, int& fd, ActorId& actor, uint32_t& op_type) {
    fd = static_cast<int>(ud & 0xFFFFFFFFULL);
    actor = ActorId(static_cast<uint32_t>((ud >> 32) & 0xFFFFULL));
    op_type = static_cast<uint32_t>((ud >> 56) & 0xFFULL);
}

} // namespace net
} // namespace hpactor
