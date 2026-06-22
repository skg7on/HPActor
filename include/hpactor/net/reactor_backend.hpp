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

#include <sys/socket.h>
#include <sys/uio.h>

namespace hpactor {
namespace net {

/// \brief I/O readiness event flags for reactor backends.
enum class IoEvent : uint32_t {
    Read = 1 << 0,  ///< File descriptor is readable.
    Write = 1 << 1, ///< File descriptor is writable.
};

/// \brief Abstract reactor (synchronous readiness) backend interface.
///
/// Reactor backends notify the caller when file descriptors become ready
/// for I/O; the caller performs the I/O synchronously in the event loop
/// thread. This is the base interface for platform-specific implementations
/// (\c EpollBackend, \c KqueueBackend, \c GcdBackend, \c IoUringBackend).
///
/// \note Thread safety: All methods are called from the event loop thread
///       only. Implementations do not need internal synchronization.
class IReactorBackend {
  public:
    virtual ~IReactorBackend() = default;

    /// \brief Initialize and start the backend.
    ///
    /// Must be called before any other method.
    /// \return \c true on success.
    virtual bool start() = 0;

    /// \brief Shut down the backend and release resources.
    virtual void stop() = 0;

    /// \brief Register a file descriptor with the given interest set.
    ///
    /// \param[in] fd File descriptor to monitor.
    /// \param[in] events Bitmask of \c IoEvent flags.
    /// \return \c true on success.
    virtual bool add_fd(int fd, IoEvent events) = 0;

    /// \brief Update the interest set for a previously registered fd.
    ///
    /// \param[in] fd File descriptor to update.
    /// \param[in] events New bitmask of \c IoEvent flags.
    /// \return \c true on success.
    virtual bool update_fd(int fd, IoEvent events) = 0;

    /// \brief Remove a file descriptor from the interest set.
    ///
    /// \param[in] fd File descriptor to deregister.
    /// \return \c true on success.
    virtual bool remove_fd(int fd) = 0;

    /// \brief Register a fixed buffer for zero-copy I/O.
    ///
    /// \param[in] addr Buffer address.
    /// \param[in] len Buffer length in bytes.
    /// \return Buffer identifier on success, negative on failure.
    /// \note Only meaningful for proactor backends (io_uring fixed buffers).
    virtual int register_buffer(const void* addr, size_t len) = 0;

    /// \brief Unregister a previously registered fixed buffer.
    ///
    /// \param[in] buffer_id Buffer identifier from \c register_buffer().
    /// \return \c true on success.
    virtual bool unregister_buffer(int buffer_id) = 0;

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

    /// \brief Wait for I/O events (blocking with timeout).
    ///
    /// \param[in] timeout_ms Maximum wait time in milliseconds.
    /// \return Number of events triggered, 0 on timeout, -1 on error.
    virtual int wait(int timeout_ms) = 0;

    /// \brief Process triggered events.
    ///
    /// Must be called after \c wait() returns a positive count.
    virtual void process_events() = 0;

    // ── Async I/O submission ────────────────────────────────────────────

    /// \brief Submit an asynchronous send operation.
    ///
    /// \param[in] fd Connected socket file descriptor.
    /// \param[in] bufs I/O vector describing the data to send.
    /// \param[in] buf_count Number of entries in \c bufs.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;

    /// \brief Submit an asynchronous receive operation.
    ///
    /// \param[in] fd Connected socket file descriptor.
    /// \param[in] bufs I/O vector describing receive buffers.
    /// \param[in] buf_count Number of entries in \c bufs.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;

    /// \brief Submit an asynchronous accept operation.
    ///
    /// \param[in] fd Listening socket file descriptor.
    /// \param[in] actor Actor to receive the completion.
    virtual void async_accept(int fd, ActorId actor) = 0;

    /// \brief Submit an asynchronous connect operation.
    ///
    /// \param[in] fd Non-blocking socket file descriptor.
    /// \param[in] addr Remote address to connect to.
    /// \param[in] addrlen Size of \c addr.
    /// \param[in] actor Actor to receive the completion.
    virtual void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) = 0;

    /// \brief Submit an asynchronous sendto operation.
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

    /// \brief Submit an asynchronous recvfrom operation.
    ///
    /// \param[in] fd Unconnected socket file descriptor.
    /// \param[in] bufs I/O vector describing receive buffers.
    /// \param[in] buf_count Number of entries in \c bufs.
    /// \param[in] actor Actor to receive the completion.
    /// \param[in] op_type Operation type for the completion record.
    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) = 0;

    // ── Handler management ──────────────────────────────────────────────

    /// \brief Register a read handler for a file descriptor.
    ///
    /// Reactor backends read data in \c wait() and dispatch via this
    /// callback. Proactor backends use \c async_recv completions instead.
    /// \param[in] fd File descriptor to watch.
    /// \param[in] handler Callback invoked when data is available.
    virtual void set_read_handler(int fd, read_callback handler) = 0;

    /// \brief Remove a read handler.
    ///
    /// \param[in] fd File descriptor whose handler should be removed.
    virtual void clear_read_handler(int fd) = 0;

    /// \brief Query whether this backend dispatches read handlers directly
    /// from \c wait().
    ///
    /// \return \c true for reactor backends (epoll, kqueue),
    ///         \c false for proactor backends (io_uring, GCD).
    virtual bool supports_read_handler() const = 0;

    /// \brief Register a write handler for a file descriptor.
    ///
    /// Used for non-blocking connect completion: the callback checks
    /// \c SO_ERROR and transitions the connection on success.
    /// \param[in] fd File descriptor to watch for writability.
    /// \param[in] handler Callback invoked when the fd becomes writable.
    virtual void set_write_handler(int fd, write_callback handler) = 0;

    /// \brief Remove a write handler.
    ///
    /// \param[in] fd File descriptor whose handler should be removed.
    virtual void clear_write_handler(int fd) = 0;

    /// \brief Query whether this backend supports write handler dispatch.
    ///
    /// \return \c true for reactor backends (epoll, kqueue),
    ///         \c false for proactor backends (io_uring, GCD).
    virtual bool supports_write_handler() const = 0;
};

} // namespace net
} // namespace hpactor
