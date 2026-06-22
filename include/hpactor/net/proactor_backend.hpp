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

namespace hpactor {
namespace net {

/// \brief Abstract proactor (true async I/O) backend interface.
///
/// Proactor backends perform I/O asynchronously and deliver completions
/// via \c deliver_completion(). Unlike reactor backends, the caller never
/// performs I/O synchronously — all operations are submitted and completed
/// out-of-band by the kernel or dispatch queue.
///
/// Implementations include \c IoUringBackend (Linux io_uring) and
/// \c GcdBackend (macOS Grand Central Dispatch).
///
/// \note Thread safety: All methods are called from the event loop thread.
class IProactorBackend {
  public:
    virtual ~IProactorBackend() = default;

    /// \brief Initialize and start the backend.
    ///
    /// \return \c true on success.
    virtual bool start() = 0;

    /// \brief Shut down the backend and release resources.
    virtual void stop() = 0;

    /// \brief Submit an asynchronous send on a connected socket.
    ///
    /// \param[in] fd Connected socket file descriptor.
    /// \param[in] bufs I/O vector describing the data to send.
    /// \param[in] buf_count Number of entries in \c bufs.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;

    /// \brief Submit an asynchronous receive on a connected socket.
    ///
    /// \param[in] fd Connected socket file descriptor.
    /// \param[in] bufs I/O vector describing receive buffers.
    /// \param[in] buf_count Number of entries in \c bufs.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;

    /// \brief Submit an asynchronous accept on a listening socket.
    ///
    /// \param[in] fd Listening socket file descriptor.
    /// \param[in] actor Actor to receive the completion.
    virtual void async_accept(int fd, ActorId actor) = 0;

    /// \brief Submit an asynchronous connect.
    ///
    /// \param[in] fd Non-blocking socket file descriptor.
    /// \param[in] addr Remote address to connect to.
    /// \param[in] addrlen Size of \c addr.
    /// \param[in] actor Actor to receive the completion.
    virtual void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) = 0;

    /// \brief Submit an asynchronous send using a pre-registered fixed buffer
    /// (zero-copy).
    ///
    /// \param[in] fd Connected socket file descriptor.
    /// \param[in] buffer_id Buffer identifier from \c register_buffer().
    /// \param[in] offset Byte offset into the fixed buffer.
    /// \param[in] len Number of bytes to send.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    /// \note Only supported by io_uring backends using registered buffers.
    virtual void async_send_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;

    /// \brief Submit an asynchronous receive into a pre-registered fixed
    /// buffer (zero-copy).
    ///
    /// \param[in] fd Connected socket file descriptor.
    /// \param[in] buffer_id Buffer identifier from \c register_buffer().
    /// \param[in] offset Byte offset into the fixed buffer.
    /// \param[in] len Maximum number of bytes to receive.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void async_recv_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;

    /// \brief Submit an asynchronous recvfrom on an unconnected socket.
    ///
    /// \param[in] fd Unconnected socket file descriptor.
    /// \param[in] bufs I/O vector describing receive buffers.
    /// \param[in] buf_count Number of entries in \c bufs.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) = 0;

    /// \brief Submit an asynchronous sendto on an unconnected socket.
    ///
    /// \param[in] fd Unconnected socket file descriptor.
    /// \param[in] bufs I/O vector describing the data to send.
    /// \param[in] buf_count Number of entries in \c bufs.
    /// \param[in] addr Destination address.
    /// \param[in] addrlen Size of \c addr.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void
    async_sendto(int fd, const iovec* bufs, int buf_count, const sockaddr* addr,
                 socklen_t addrlen, ActorId actor, uint32_t op_type) = 0;

    /// \brief Schedule a one-shot timer.
    ///
    /// \param[in] actor Actor to notify on expiry.
    /// \param[in] delay_ms Delay in milliseconds.
    /// \return Timer handle for cancellation.
    virtual uint64_t run_after(ActorId actor, int delay_ms) = 0;

    /// \brief Schedule a repeating timer.
    ///
    /// \param[in] actor Actor to notify on each expiry.
    /// \param[in] interval_ms Interval in milliseconds.
    /// \return Timer handle for cancellation.
    virtual uint64_t run_every(ActorId actor, int interval_ms) = 0;

    /// \brief Cancel a previously scheduled timer.
    ///
    /// \param[in] handle Timer handle from \c run_after() or
    ///            \c run_every().
    virtual void cancel_timer(uint64_t handle) = 0;

    /// \brief Wait for completions (blocking with timeout).
    ///
    /// \param[in] timeout_ms Maximum wait time in milliseconds.
    /// \return Number of completions available, 0 on timeout, -1 on error.
    virtual int wait(int timeout_ms) = 0;

    /// \brief Process and dispatch all pending completions.
    virtual void process_completions() = 0;

    /// \brief Deliver a single completion to the actor system.
    ///
    /// Called internally by the backend when an async operation finishes.
    /// \param[in] completion The completion record.
    /// \note Thread safety: Called from the event loop thread.
    virtual void deliver_completion(OpCompletion completion) = 0;
};

} // namespace net
} // namespace hpactor
