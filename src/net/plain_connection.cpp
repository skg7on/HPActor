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

PlainConnection::PlainConnection(CommunicationEndpoint remote_endpoint,
                                 EventLoop* loop, int socket_fd)
    : Connection(remote_endpoint), remote_endpoint_(remote_endpoint),
      loop_(loop), fd_(socket_fd) {}

PlainConnection::~PlainConnection() {
    close();
}

PlainConnectionPtr
PlainConnection::create_client(int fd, CommunicationEndpoint remote_endpoint,
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
            loop->set_read_handler(fd, [weak_conn](const bytes& data) {
                if (auto self = weak_conn.lock()) {
                    self->handle_read(data);
                }
            });
        }
    }

    return conn;
}

PlainConnectionPtr
PlainConnection::create_server(int fd, CommunicationEndpoint remote_endpoint,
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
            loop->set_read_handler(fd, [weak_conn](const bytes& data) {
                if (auto self = weak_conn.lock()) {
                    self->handle_read(data);
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

void PlainConnection::send(const bytes& frame_data) {
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

void PlainConnection::handle_read(const bytes& data) {
    read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());

    // Process complete frames
    // Simple framing: 4-byte length header
    while (read_buffer_.size() >= 4) {
        size_t frame_len = (static_cast<size_t>(read_buffer_[0]) << 24) |
                           (static_cast<size_t>(read_buffer_[1]) << 16) |
                           (static_cast<size_t>(read_buffer_[2]) << 8) |
                           static_cast<size_t>(read_buffer_[3]);

        if (read_buffer_.size() < 4 + frame_len) {
            break; // Wait for more data
        }

        bytes frame(read_buffer_.begin() + static_cast<long>(4),
                    read_buffer_.begin() + static_cast<long>(4 + frame_len));
        read_buffer_.erase(read_buffer_.begin(),
                           read_buffer_.begin() + static_cast<long>(4 + frame_len));

        if (frame_handler_) {
            frame_handler_(frame);
        }
    }
}

void PlainConnection::send_raw(const bytes& data) {
    if (fd_ < 0 || !loop_)
        return;

    // Append data to write buffer
    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

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

    // Remove sent bytes from write buffer
    if (static_cast<size_t>(result) >= write_buffer_.size()) {
        write_buffer_.clear();
    } else {
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + result);
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