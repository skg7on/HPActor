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

#include <cstdint>
#include <memory>

namespace hpactor::python {

/// \brief Non-blocking platform notifier for waking the Python asyncio event
///        loop.
///
/// On Linux, backed by a single \c eventfd with \c EFD_NONBLOCK and
/// \c EFD_CLOEXEC. On macOS (and other non-Linux POSIX systems), backed by a
/// \c socketpair with \c O_NONBLOCK and \c FD_CLOEXEC on both descriptors.
///
/// The notifier is move-only. After a move, the source becomes invalid. After
/// \c close(), the notifier is invalid and \c signal() becomes a no-op that
/// returns \c false.
class NativeNotifier final {
  public:
    /// \brief Create a new notifier.
    ///
    /// \return A result containing a unique_ptr to a valid NativeNotifier, or
    ///         an error if the underlying platform primitive could not be
    ///         created.
    static result<std::unique_ptr<NativeNotifier>> create() noexcept;

    NativeNotifier(NativeNotifier&& other) noexcept;
    NativeNotifier& operator=(NativeNotifier&& other) noexcept;
    ~NativeNotifier();

    NativeNotifier(const NativeNotifier&) = delete;
    NativeNotifier& operator=(const NativeNotifier&) = delete;

    /// \brief Check whether this notifier holds a valid file descriptor.
    /// \return true if the notifier is open and usable.
    [[nodiscard]] bool valid() const noexcept;

    /// \brief Return the read-end file descriptor for use with epoll/kqueue.
    /// \return A non-negative file descriptor, or -1 after close or move.
    [[nodiscard]] int read_fd() const noexcept;

    /// \brief Signal the event loop that dispatch/completion data is available.
    ///
    /// Writes a wakeup token. If the underlying primitive's buffer is full
    /// (\c EAGAIN), a wakeup is already pending so the call is treated as
    /// successful.
    ///
    /// \return true if the signal was delivered or already pending, false on
    ///         error or if the notifier has been closed or moved from.
    [[nodiscard]] bool signal() noexcept;

    /// \brief Drain all pending wakeup tokens.
    ///
    /// Loops until \c EAGAIN, summing the accumulated count. Never blocks.
    ///
    /// \return The number of wakeups drained (may be zero).
    [[nodiscard]] uint64_t drain() noexcept;

    /// \brief Close the underlying file descriptor(s).
    ///
    /// Idempotent: safe to call multiple times. On Linux, the shared eventfd
    /// descriptor is closed exactly once. After calling \c close(), \c valid()
    /// returns \c false and \c signal() returns \c false.
    void close() noexcept;

  private:
    NativeNotifier(int read_fd, int write_fd) noexcept;

    int read_fd_{-1};
    int write_fd_{-1};
};

} // namespace hpactor::python
