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

#include <hpactor/net/wireframe_connection.hpp>

#include <hpactor/net/event_loop.hpp>

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace hpactor {

namespace net {

WireFrameConnection::WireFrameConnection(int fd, EndPoint local_endpoint,
                                         EndPoint remote_endpoint, EventLoop* loop)
    : Connection(fd, local_endpoint, remote_endpoint, loop), is_sending_(false) {
    write_buffer_.reserve(kWriteChunkSize);
}

WireFrameConnection::~WireFrameConnection() {
    close();
}

WireFrameConnectionPtr
WireFrameConnection::create_as_client(int fd, EndPoint local_endpoint,
                                      EndPoint remote_endpoint, EventLoop* loop) {
    auto conn = std::shared_ptr<WireFrameConnection>(
        new WireFrameConnection(fd, local_endpoint, remote_endpoint, loop));
    conn->set_state(ConnectionState::Connected);

    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
        if (loop->supports_read_handler()) {
            std::weak_ptr<WireFrameConnection> weak_conn = conn;
            loop->set_read_handler(fd, [weak_conn](int /*event_fd*/) {
                if (auto self = weak_conn.lock()) {
                    self->handle_read();
                }
            });
        }
    }

    return conn;
}

WireFrameConnectionPtr
WireFrameConnection::create_connecting_client(int fd, EndPoint local_endpoint,
                                              EndPoint remote_endpoint,
                                              EventLoop* loop) {
    auto conn = std::shared_ptr<WireFrameConnection>(
        new WireFrameConnection(fd, local_endpoint, remote_endpoint, loop));
    conn->set_state(ConnectionState::Connecting);
    // Event loop registration deferred — caller must verify connect and call
    // setup_after_connect().
    return conn;
}

void WireFrameConnection::setup_after_connect(WireFrameConnectionPtr conn) {
    conn->set_state(ConnectionState::Connected);

    auto* loop = conn->event_loop();
    int fd = conn->fd();
    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
        if (loop->supports_read_handler()) {
            std::weak_ptr<WireFrameConnection> weak_conn = conn;
            loop->set_read_handler(fd, [weak_conn](int /*event_fd*/) {
                if (auto self = weak_conn.lock()) {
                    self->handle_read();
                }
            });
        }
    }
}

WireFrameConnectionPtr
WireFrameConnection::create_as_server(int fd, EndPoint local_endpoint,
                                      EndPoint remote_endpoint, EventLoop* loop) {
    auto conn = std::shared_ptr<WireFrameConnection>(
        new WireFrameConnection(fd, local_endpoint, remote_endpoint, loop));
    conn->set_state(ConnectionState::Connected);

    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
        if (loop->supports_read_handler()) {
            std::weak_ptr<WireFrameConnection> weak_conn = conn;
            loop->set_read_handler(fd, [weak_conn](int /*event_fd*/) {
                if (auto self = weak_conn.lock()) {
                    self->handle_read();
                }
            });
        }
    }

    return conn;
}

void WireFrameConnection::set_ready_handler(std::function<void(ConnectionPtr)> handler) {
    ready_handler_ = std::move(handler);
}

void WireFrameConnection::set_frame_handler(frame_handler handler) {
    frame_handler_ = std::move(handler);
}

void WireFrameConnection::set_error_handler(
    std::function<void(ConnectionPtr, const error&)> handler) {
    error_handler_ = std::move(handler);
}

void WireFrameConnection::set_send_completion_handler(
    std::function<void(int result)> handler) {
    send_completion_handler_ = std::move(handler);
}

void WireFrameConnection::send(const StreamBuffer& frame_data) {
    if (state_ != ConnectionState::Connected) {
        return;
    }
    send_raw(frame_data);
}

void WireFrameConnection::close() {
    if (fd_ >= 0) {
        if (loop_) {
            loop_->clear_read_handler(fd_);
            loop_->remove_fd(fd_);
        }
        ::close(fd_);
        fd_ = -1;
    }
    set_state(ConnectionState::Disconnected);
}

void WireFrameConnection::handle_read() {
    // Phase 1 — accumulate header bytes (8 bytes: magic + payload length).
    // Read header first so we know the exact payload length before allocating
    // buffer space for it.
    while (read_buffer_.size() < WireFrame::HeaderSize) {
        size_t remaining = WireFrame::HeaderSize - read_buffer_.size();
        uint8_t* area = read_buffer_.reserve_tail(remaining);
        ssize_t n = ::read(fd_, area, remaining);
        if (n > 0) {
            read_buffer_.commit_tail(static_cast<size_t>(n));
        } else if (n == 0) {
            read_buffer_.clear(); // EOF — discard incomplete header
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            read_buffer_.clear(); // Error — discard partial data
            return;
        }
    }

    // Validate magic "HPAC"
    if (read_buffer_[0] != 'H' || read_buffer_[1] != 'P' ||
        read_buffer_[2] != 'A' || read_buffer_[3] != 'C') {
        // Invalid magic — skip one byte and retry (re-sync stream)
        read_buffer_.consume(1);
        handle_read(); // Recurse to re-enter header-read phase
        return;
    }

    // Parse remaining length (big-endian uint32_t from bytes 4-7)
    size_t payload_len = (static_cast<size_t>(read_buffer_[4]) << 24) |
                         (static_cast<size_t>(read_buffer_[5]) << 16) |
                         (static_cast<size_t>(read_buffer_[6]) << 8) |
                         static_cast<size_t>(read_buffer_[7]);

    size_t total_frame_size = WireFrame::HeaderSize + payload_len;

    // Phase 2 — accumulate exactly payload_len bytes.
    // Reserve only the remaining bytes needed, not a fixed chunk.
    while (read_buffer_.size() < total_frame_size) {
        size_t remaining = total_frame_size - read_buffer_.size();
        uint8_t* area = read_buffer_.reserve_tail(remaining);
        ssize_t n = ::read(fd_, area, remaining);
        if (n > 0) {
            read_buffer_.commit_tail(static_cast<size_t>(n));
        } else if (n == 0) {
            read_buffer_.clear(); // EOF mid-frame — discard incomplete frame
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            read_buffer_.clear(); // Error — discard partial data
            return;
        }
    }

    // Complete frame received — extract into StreamBuffer.
    StreamBuffer frame_data(read_buffer_.begin(),
                            read_buffer_.begin() + total_frame_size);
    read_buffer_.consume(total_frame_size);

    if (frame_handler_) {
        frame_handler_(std::move(frame_data));
    }

    // Process any additional frames already in the buffer.
    if (read_buffer_.size() >= WireFrame::HeaderSize) {
        handle_read();
    }
}

void WireFrameConnection::send_raw(const StreamBuffer& data) {
    if (fd_ < 0 || !loop_)
        return;

    write_buffer_.append(data.data(), data.size());

    if (is_sending_)
        return;

    flush_write_buffer();
}

void WireFrameConnection::flush_write_buffer() {
    if (fd_ < 0 || loop_ == nullptr || write_buffer_.empty()) {
        return;
    }

    is_sending_ = true;

    struct iovec iov;
    iov.iov_base = write_buffer_.data();
    iov.iov_len = write_buffer_.size();

    loop_->backend()->async_send(fd_, &iov, 1, ActorId(0),
                                 static_cast<uint32_t>(OpType::Send));
}

void WireFrameConnection::handle_send_completion(int result) {
    if (send_completion_handler_) {
        send_completion_handler_(result);
    }
    is_sending_ = false;

    if (result < 0) {
        set_state(ConnectionState::Error);
        if (error_handler_) {
            error_handler_(
                std::enable_shared_from_this<WireFrameConnection>::shared_from_this(),
                error{});
        }
        return;
    }

    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.consume(static_cast<size_t>(result));
    }

    if (!write_buffer_.empty()) {
        flush_write_buffer();
    }
}

} // namespace net
} // namespace hpactor
