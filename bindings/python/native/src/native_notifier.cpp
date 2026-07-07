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

#include <hpactor/python/native_notifier.hpp>

#include <cerrno>
#include <cstring>

#ifdef __linux__
#    include <sys/eventfd.h>
#    include <unistd.h>
#else
#    include <fcntl.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace hpactor::python {

namespace {

#ifdef __linux__
/// Value written to the eventfd on each signal.
constexpr uint64_t kSignalValue = 1;
#else
/// Byte written to the socketpair on each signal.
constexpr uint8_t kSignalByte = 0x01;
#endif

/// Build an error with the last errno and a descriptive message.
[[nodiscard]] error make_syscall_error(const char* call) noexcept {
    return error(errors::unknown, std::string(call) + ": " + std::strerror(errno));
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------------

NativeNotifier::NativeNotifier(int read_fd, int write_fd) noexcept
    : read_fd_(read_fd), write_fd_(write_fd) {}

NativeNotifier::NativeNotifier(NativeNotifier&& other) noexcept
    : read_fd_(other.read_fd_), write_fd_(other.write_fd_) {
    other.read_fd_ = -1;
    other.write_fd_ = -1;
}

NativeNotifier& NativeNotifier::operator=(NativeNotifier&& other) noexcept {
    if (this != &other) {
        close();
        read_fd_ = other.read_fd_;
        write_fd_ = other.write_fd_;
        other.read_fd_ = -1;
        other.write_fd_ = -1;
    }
    return *this;
}

NativeNotifier::~NativeNotifier() {
    close();
}

// -----------------------------------------------------------------------------
// Observers
// -----------------------------------------------------------------------------

bool NativeNotifier::valid() const noexcept {
    return read_fd_ >= 0;
}

int NativeNotifier::read_fd() const noexcept {
    return read_fd_;
}

// -----------------------------------------------------------------------------
// Signal / drain
// -----------------------------------------------------------------------------

bool NativeNotifier::signal() noexcept {
    if (!valid()) {
        return false;
    }
#ifdef __linux__
    uint64_t value = kSignalValue;
    ssize_t ret;
    do {
        ret = ::write(write_fd_, &value, sizeof(value));
    } while (ret == -1 && errno == EINTR);
    if (ret == -1 && errno == EAGAIN) {
        return true; // wakeup already pending
    }
    return ret == static_cast<ssize_t>(sizeof(value));
#else
    uint8_t byte = kSignalByte;
    ssize_t ret;
    do {
        ret = ::write(write_fd_, &byte, sizeof(byte));
    } while (ret == -1 && errno == EINTR);
    if (ret == -1 && errno == EAGAIN) {
        return true;
    }
    return ret == static_cast<ssize_t>(sizeof(byte));
#endif
}

uint64_t NativeNotifier::drain() noexcept {
    if (!valid()) {
        return 0;
    }
    uint64_t count = 0;
#ifdef __linux__
    uint64_t buf;
    for (;;) {
        ssize_t ret;
        do {
            ret = ::read(read_fd_, &buf, sizeof(buf));
        } while (ret == -1 && errno == EINTR);
        if (ret == -1 && errno == EAGAIN) {
            break;
        }
        if (ret != static_cast<ssize_t>(sizeof(buf))) {
            break;
        }
        count += buf;
    }
#else
    uint8_t byte;
    for (;;) {
        ssize_t ret;
        do {
            ret = ::read(read_fd_, &byte, sizeof(byte));
        } while (ret == -1 && errno == EINTR);
        if (ret == -1 && errno == EAGAIN) {
            break;
        }
        if (ret != static_cast<ssize_t>(sizeof(byte))) {
            break;
        }
        ++count;
    }
#endif
    return count;
}

void NativeNotifier::close() noexcept {
    if (!valid()) {
        return;
    }
    int fd = read_fd_;
    read_fd_ = -1;
#ifdef __linux__
    // Linux: read_fd_ == write_fd_ (same eventfd), close exactly once.
    write_fd_ = -1;
    ::close(fd);
#else
    // macOS / other: two ends of a socketpair.
    int wfd = write_fd_;
    write_fd_ = -1;
    if (wfd >= 0 && wfd != fd) {
        ::close(wfd);
    }
    ::close(fd);
#endif
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

result<std::unique_ptr<NativeNotifier>> NativeNotifier::create() noexcept {
#ifdef __linux__
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        return result<std::unique_ptr<NativeNotifier>>::make(
            make_syscall_error("eventfd"));
    }
    // On Linux, the same descriptor serves as both read and write end.
    auto ptr = std::unique_ptr<NativeNotifier>(new NativeNotifier(fd, fd));
    return result<std::unique_ptr<NativeNotifier>>::make(std::move(ptr));
#else
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return result<std::unique_ptr<NativeNotifier>>::make(
            make_syscall_error("socketpair"));
    }
    // Set O_NONBLOCK and FD_CLOEXEC on both descriptors.
    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(fds[i], F_GETFD);
        if (flags >= 0) {
            ::fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC);
        }
        flags = ::fcntl(fds[i], F_GETFL);
        if (flags >= 0) {
            ::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    auto ptr = std::unique_ptr<NativeNotifier>(new NativeNotifier(fds[0], fds[1]));
    return result<std::unique_ptr<NativeNotifier>>::make(std::move(ptr));
#endif
}

} // namespace hpactor::python
