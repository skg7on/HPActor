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

#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/reactor_backend.hpp>
#include <hpactor/types/types.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace net {

/// \brief Dispatches reactor (sync I/O) readiness events to actor
/// mailboxes.
///
/// In reactor mode, the backend signals "fd is ready for I/O" and this
/// dispatcher performs the synchronous I/O operation and delivers a
/// completion event to the actor's mailbox. Tracks pending operations
/// per file descriptor and performs the actual \c readv(), \c send(),
/// \c accept(), or \c connect() calls synchronously from the event loop
/// thread.
///
/// \note Thread safety: Called from the event loop thread only. Not
///       internally synchronized.
class ReactorDispatcher {
  public:
    // Completion handler callback - called when I/O completes
    using completion_handler = std::function<void(OpCompletion)>;

    void set_completion_handler(completion_handler handler) {
        handler_ = std::move(handler);
    }

    // Register an fd-to-actor mapping without active I/O tracking.
    // Use register_recv/register_send/etc. for operations that need
    // I/O performed on readiness.
    void register_fd(int fd, ActorId actor) {
        fd_to_actor_[fd] = actor;
    }

    void unregister_fd(int fd) {
        fd_to_actor_.erase(fd);
        pending_ops_.erase(fd);
    }

    // Pending I/O operation tracked per fd
    struct PendingIO {
        ActorId actor;
        OpType type;
        int buf_count = 0;
        iovec saved_bufs[16];       // buffers for recv operations
        std::vector<uint8_t> data;  // concatenated data for send operations
        sockaddr_storage addr = {}; // target address for sendto/recvfrom
        socklen_t addrlen = 0;
    };

    // Register pending I/O operations.
    // The dispatcher will perform the I/O when on_readiness fires.

    void
    register_recv(int fd, ActorId actor, OpType type, iovec* bufs, int buf_count) {
        PendingIO op;
        op.actor = actor;
        op.type = type;
        op.buf_count = buf_count;
        for (int i = 0; i < buf_count && i < 16; ++i) {
            op.saved_bufs[i] = bufs[i];
        }
        fd_to_actor_[fd] = actor;
        pending_ops_[fd] = std::move(op);
    }

    void register_send(int fd, ActorId actor, OpType type, const iovec* bufs,
                       int buf_count) {
        PendingIO op;
        op.actor = actor;
        op.type = type;
        // Concatenate buffer data for send
        size_t total = 0;
        for (int i = 0; i < buf_count; ++i) {
            total += bufs[i].iov_len;
        }
        op.data.resize(total);
        size_t offset = 0;
        for (int i = 0; i < buf_count; ++i) {
            if (bufs[i].iov_base && bufs[i].iov_len > 0) {
                std::memcpy(op.data.data() + offset, bufs[i].iov_base,
                            bufs[i].iov_len);
                offset += bufs[i].iov_len;
            }
        }
        fd_to_actor_[fd] = actor;
        pending_ops_[fd] = std::move(op);
    }

    void register_sendto(int fd, ActorId actor, OpType type, const iovec* bufs,
                         int buf_count, const sockaddr* addr, socklen_t addrlen) {
        PendingIO op;
        op.actor = actor;
        op.type = type;
        size_t total = 0;
        for (int i = 0; i < buf_count; ++i) {
            total += bufs[i].iov_len;
        }
        op.data.resize(total);
        size_t offset = 0;
        for (int i = 0; i < buf_count; ++i) {
            if (bufs[i].iov_base && bufs[i].iov_len > 0) {
                std::memcpy(op.data.data() + offset, bufs[i].iov_base,
                            bufs[i].iov_len);
                offset += bufs[i].iov_len;
            }
        }
        if (addr && addrlen > 0) {
            std::memcpy(&op.addr, addr, addrlen);
            op.addrlen = addrlen;
        }
        fd_to_actor_[fd] = actor;
        pending_ops_[fd] = std::move(op);
    }

    void register_accept(int fd, ActorId actor) {
        PendingIO op;
        op.actor = actor;
        op.type = OpType::Accept;
        fd_to_actor_[fd] = actor;
        pending_ops_[fd] = std::move(op);
    }

    void register_connect(int fd, ActorId actor) {
        PendingIO op;
        op.actor = actor;
        op.type = OpType::Connect;
        fd_to_actor_[fd] = actor;
        pending_ops_[fd] = std::move(op);
    }

    // Remove pending I/O tracking without removing the fd mapping
    void unregister_io(int fd) {
        pending_ops_.erase(fd);
    }

    bool has_pending(int fd) const {
        return pending_ops_.find(fd) != pending_ops_.end();
    }

    // Called by the reactor backend when an fd is ready for I/O.
    // Performs the pending I/O operation and delivers a completion.
    void on_readiness(int fd, IoEvent events) {
        auto it = pending_ops_.find(fd);
        if (it == pending_ops_.end()) {
            return; // No pending operation for this fd
        }

        PendingIO& op = it->second;
        OpCompletion completion{};
        completion.actor = op.actor;
        completion.fd = fd;
        completion.user_data = 0;

        switch (op.type) {
            case OpType::Recv:
                if (int(events) & int(IoEvent::Read)) {
                    do_recv(completion, fd, op);
                }
                break;
            case OpType::RecvFrom:
                if (int(events) & int(IoEvent::Read)) {
                    do_recvfrom(completion, fd, op);
                }
                break;
            case OpType::Send:
                if (int(events) & int(IoEvent::Write)) {
                    do_send(completion, fd, op);
                }
                break;
            case OpType::SendTo:
                if (int(events) & int(IoEvent::Write)) {
                    do_sendto(completion, fd, op);
                }
                break;
            case OpType::Accept:
                if (int(events) & int(IoEvent::Read)) {
                    do_accept(completion, fd);
                }
                break;
            case OpType::Connect:
                if (int(events) & int(IoEvent::Write)) {
                    do_connect(completion, fd);
                }
                break;
            default:
                break;
        }

        // If the operation completed (not still pending), deliver and remove
        if (completion.type != OpType::TimerFired) {
            pending_ops_.erase(it);
            if (handler_) {
                handler_(completion);
            }
        }
    }

  private:
    void do_recv(OpCompletion& completion, int fd, PendingIO& op) {
        ssize_t total_n = 0;
        int last_err = 0;

        // Drain loop for edge-triggered readiness
        while (true) {
            ssize_t n = ::readv(fd, op.saved_bufs, op.buf_count);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // No more data
                }
                last_err = errno;
                break;
            }
            total_n += n;
            if (n == 0) {
                break; // EOF
            }
            // Check if we filled all buffers
            break;
        }

        completion.type = OpType::Recv;
        completion.result = (last_err != 0) ? -last_err : static_cast<int>(total_n);
    }

    void do_recvfrom(OpCompletion& completion, int fd, PendingIO& op) {
        ssize_t total_n = 0;
        int last_err = 0;
        socklen_t addrlen = sizeof(op.addr);

        while (true) {
            struct msghdr msg;
            std::memset(&msg, 0, sizeof(msg));
            msg.msg_name = &op.addr;
            msg.msg_namelen = addrlen;
            msg.msg_iov = op.saved_bufs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(op.buf_count);

            ssize_t n = ::recvmsg(fd, &msg, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                last_err = errno;
                break;
            }
            total_n += n;
            completion.src_addr_len = msg.msg_namelen;
            break;
        }

        completion.type = OpType::RecvFrom;
        completion.result = (last_err != 0) ? -last_err : static_cast<int>(total_n);
        if (total_n > 0) {
            std::memcpy(&completion.src_addr, &op.addr, sizeof(completion.src_addr));
        }
    }

    void do_send(OpCompletion& completion, int fd, PendingIO& op) {
        ssize_t total_n = 0;
        int last_err = 0;

        while (total_n < static_cast<ssize_t>(op.data.size())) {
            ssize_t n = ::send(fd, op.data.data() + total_n,
                               op.data.size() - static_cast<size_t>(total_n), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (total_n > 0) {
                        break; // Partial send, report what we sent
                    }
                    last_err = EAGAIN;
                    break;
                }
                last_err = errno;
                break;
            }
            total_n += n;
        }

        completion.type = OpType::Send;
        completion.result = (last_err != 0) ? -last_err : static_cast<int>(total_n);
    }

    void do_sendto(OpCompletion& completion, int fd, PendingIO& op) {
        ssize_t total_n = 0;
        int last_err = 0;

        while (total_n < static_cast<ssize_t>(op.data.size())) {
            ssize_t n = ::sendto(fd, op.data.data() + total_n,
                                 op.data.size() - static_cast<size_t>(total_n),
                                 0, reinterpret_cast<const sockaddr*>(&op.addr),
                                 op.addrlen);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (total_n > 0) {
                        break;
                    }
                    last_err = EAGAIN;
                    break;
                }
                last_err = errno;
                break;
            }
            total_n += n;
        }

        completion.type = OpType::SendTo;
        completion.result = (last_err != 0) ? -last_err : static_cast<int>(total_n);
    }

    void do_accept(OpCompletion& completion, int fd) {
        // Ensure non-blocking
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0 && !(flags & O_NONBLOCK)) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int client_fd = ::accept(fd, nullptr, nullptr);
        if (client_fd >= 0) {
            // Set client fd to non-blocking
            int cflags = fcntl(client_fd, F_GETFL, 0);
            if (cflags >= 0) {
                fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK);
            }
        }

        completion.type = OpType::Accept;
        completion.result = (client_fd >= 0) ? client_fd : -errno;
        completion.fd = (client_fd >= 0) ? client_fd : -1;
    }

    void do_connect(OpCompletion& completion, int fd) {
        // Check connect result via SO_ERROR
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
            error = errno;
        }

        completion.type = OpType::Connect;
        completion.result = (error == 0) ? 0 : -error;
    }

    std::unordered_map<int, ActorId> fd_to_actor_;
    std::unordered_map<int, PendingIO> pending_ops_;
    completion_handler handler_;
};

} // namespace net
} // namespace hpactor