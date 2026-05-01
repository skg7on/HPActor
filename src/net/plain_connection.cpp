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

#include <hpactor/net/plain_connection.hpp>

#include <hpactor/net/event_loop.hpp>

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace hpactor {

namespace net {

PlainConnection::PlainConnection(EndPoint remote_endpoint,
                                 EventLoop* loop, int socket_fd)
    : Connection(remote_endpoint), remote_endpoint_(remote_endpoint),
      loop_(loop), fd_(socket_fd) {
    read_buffer_.reserve(kReadChunkSize);
    write_buffer_.reserve(kReadChunkSize);
}

PlainConnection::~PlainConnection() {
    close();
}

PlainConnectionPtr
PlainConnection::create_client(int fd, EndPoint remote_endpoint,
                               EventLoop* loop) {
    auto conn = std::shared_ptr<PlainConnection>(
        new PlainConnection(remote_endpoint, loop, fd));
    conn->set_state(ConnectionState::Connected);
    conn->is_sending_ = false;

    // Register fd with event loop for read events
    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
        if (loop->supports_read_handler()) {
            std::weak_ptr<PlainConnection> weak_conn = conn;
            loop->set_read_handler(fd, [weak_conn](int event_fd) {
                if (auto self = weak_conn.lock()) {
                    self->on_fd_readable(event_fd);
                }
            });
        }
    }

    return conn;
}

PlainConnectionPtr
PlainConnection::create_server(int fd, EndPoint remote_endpoint,
                               EventLoop* loop) {
    auto conn = std::shared_ptr<PlainConnection>(
        new PlainConnection(remote_endpoint, loop, fd));
    conn->set_state(ConnectionState::Connected);
    conn->is_sending_ = false;

    // Register fd with event loop for read events
    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
        if (loop->supports_read_handler()) {
            std::weak_ptr<PlainConnection> weak_conn = conn;
            loop->set_read_handler(fd, [weak_conn](int event_fd) {
                if (auto self = weak_conn.lock()) {
                    self->on_fd_readable(event_fd);
                }
            });
        }
    }

    return conn;
}

void PlainConnection::set_ready_handler(std::function<void(ConnectionPtr)> handler) {
    ready_handler_ = std::move(handler);
}

void PlainConnection::set_frame_handler(frame_handler handler) {
    frame_handler_ = std::move(handler);
}

void PlainConnection::set_error_handler(
    std::function<void(ConnectionPtr, const error&)> handler) {
    error_handler_ = std::move(handler);
}

void PlainConnection::set_send_completion_handler(std::function<void(int result)> handler) {
    send_completion_handler_ = std::move(handler);
}

void PlainConnection::send(const StreamBuffer& frame_data) {
    if (state_ != ConnectionState::Connected) {
        return;
    }
    send_raw(frame_data);
}

void PlainConnection::close() {
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

void PlainConnection::on_fd_readable(int fd) {
    // Read directly into the accumulation buffer via reserve_tail/commit_tail.
    while (true) {
        uint8_t* area = read_buffer_.reserve_tail(kReadChunkSize);
        ssize_t n = ::read(fd, area, kReadChunkSize);
        if (n > 0) {
            read_buffer_.commit_tail(static_cast<size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
    }

    // Zero-copy: extract complete frames as span views into read_buffer_.
    size_t offset = 0;
    while (read_buffer_.size() - offset >= 4) {
        size_t frame_len = (static_cast<size_t>(read_buffer_[offset]) << 24) |
                           (static_cast<size_t>(read_buffer_[offset + 1]) << 16) |
                           (static_cast<size_t>(read_buffer_[offset + 2]) << 8) |
                           static_cast<size_t>(read_buffer_[offset + 3]);

        if (read_buffer_.size() - offset < 4 + frame_len) {
            break;
        }

        std::span<const uint8_t> frame(read_buffer_.data() + offset + 4,
                                       frame_len);
        offset += 4 + frame_len;

        if (frame_handler_) {
            frame_handler_(frame);
        }
    }
    if (offset > 0) {
        read_buffer_.consume(offset);
    }
}

void PlainConnection::send_raw(const StreamBuffer& data) {
    if (fd_ < 0 || !loop_)
        return;

    // Append data to write buffer
    write_buffer_.append(data.data(), data.size());

    // If already sending, wait for completion
    if (is_sending_)
        return;

    flush_write_buffer();
}

void PlainConnection::flush_write_buffer() {
    if (fd_ < 0 || loop_ == nullptr || write_buffer_.empty()) {
        return;
    }

    is_sending_ = true;

    struct iovec iov;
    iov.iov_base = write_buffer_.data();
    iov.iov_len = write_buffer_.size();

    // Use async_send - completion will be delivered via loop's completion
    // callback
    loop_->backend()->async_send(fd_, &iov, 1, ActorId(0),
                                 static_cast<uint32_t>(OpType::Send));
}

void PlainConnection::handle_send_completion(int result) {
    if (send_completion_handler_) {
        send_completion_handler_(result);
    }
    is_sending_ = false;

    if (result < 0) {
        // Send error - close connection
        set_state(ConnectionState::Error);
        if (error_handler_) {
            error_handler_(
                std::enable_shared_from_this<PlainConnection>::shared_from_this(),
                error{});
        }
        return;
    }

    // Remove sent StreamBuffer from write buffer
    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.consume(static_cast<size_t>(result));
    }

    // If more data to send, continue flushing
    if (!write_buffer_.empty()) {
        flush_write_buffer();
    }
}

void PlainConnection::set_state(ConnectionState new_state) {
    state_ = new_state;
}

} // namespace net
} // namespace hpactor