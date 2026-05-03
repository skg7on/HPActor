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

#include <hpactor/net/http_connection.hpp>

#include <hpactor/net/event_loop.hpp>

#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace hpactor {
namespace net {

HTTPConnection::HTTPConnection(int fd, EndPoint local_endpoint,
                               EndPoint remote_endpoint, EventLoop* loop,
                               HTTPConnectionMode mode)
    : Connection(fd, local_endpoint, remote_endpoint, loop),
      is_sending_(false),
      mode_(mode) {
    write_buffer_.reserve(kWriteChunkSize);

    auto parser_mode = (mode == HTTPConnectionMode::Server)
                           ? HttpParserMode::Request
                           : HttpParserMode::Response;
    parser_ = std::make_unique<HttpParser>(parser_mode);

    // Wire up parser callbacks based on mode.
    if (mode == HTTPConnectionMode::Server) {
        parser_->set_on_message(
            [this](HttpRequest&& req) {
                if (request_handler_) {
                    request_handler_(this, std::move(req));
                }
            });
    } else {
        parser_->set_on_response(
            [this](int status_code, const std::vector<HttpHeader>& headers,
                   const StreamBuffer& body) {
                if (response_handler_) {
                    response_handler_(this, status_code, headers, body);
                }
            });
    }

    parser_->set_on_error([this](llhttp_errno_t /*err*/, const char* msg) {
        if (error_handler_) {
            error handler_err(errors::http_parse_error,
                              std::string(msg ? msg : "parse error"));
            error_handler_(this, handler_err);
        }
    });
}

HTTPConnection::~HTTPConnection() {
    close();
}

HTTPConnectionPtr
HTTPConnection::create(int fd, EndPoint local_endpoint,
                       EndPoint remote_endpoint, EventLoop* loop,
                       HTTPConnectionMode mode) {
    auto conn = std::shared_ptr<HTTPConnection>(
        new HTTPConnection(fd, local_endpoint, remote_endpoint, loop, mode));
    conn->set_state(ConnectionState::Connected);

    if (loop && fd >= 0) {
        loop->add_fd(fd, EventLoop::Event::Read);
        if (loop->supports_read_handler()) {
            std::weak_ptr<HTTPConnection> weak_conn = conn;
            loop->set_read_handler(fd, [weak_conn](int /*event_fd*/) {
                if (auto self = weak_conn.lock()) {
                    self->handle_read();
                }
            });
        }
    }

    return conn;
}

void HTTPConnection::send(const StreamBuffer& data) {
    if (state_ != ConnectionState::Connected) {
        return;
    }
    send_raw(data);
}

void HTTPConnection::send_raw(const StreamBuffer& data) {
    if (fd_ < 0 || !loop_)
        return;

    write_buffer_.append(data.data(), data.size());

    if (is_sending_)
        return;

    flush_write_buffer();
}

void HTTPConnection::send_response(HttpStatusCode code,
                                   std::vector<HttpHeader> headers,
                                   StreamBuffer body) {
    // Build HTTP/1.1 status line.
    char status_line[64];
    int status_line_len = snprintf(status_line, sizeof(status_line),
                                   "HTTP/1.1 %d %s\r\n",
                                   static_cast<int>(code),
                                   reason_phrase(code));

    StreamBuffer response;
    response.append(reinterpret_cast<const uint8_t*>(status_line),
                    static_cast<size_t>(status_line_len));

    // Add Content-Length header.
    char content_length[32];
    int cl_len = snprintf(content_length, sizeof(content_length),
                          "Content-Length: %zu\r\n", body.size());
    response.append(reinterpret_cast<const uint8_t*>(content_length),
                    static_cast<size_t>(cl_len));

    // Add caller-supplied headers.
    for (const auto& h : headers) {
        response.append(reinterpret_cast<const uint8_t*>(h.name.data()),
                        h.name.size());
        response.append(reinterpret_cast<const uint8_t*>(": "), 2);
        response.append(reinterpret_cast<const uint8_t*>(h.value.data()),
                        h.value.size());
        response.append(reinterpret_cast<const uint8_t*>("\r\n"), 2);
    }

    // End of headers.
    response.append(reinterpret_cast<const uint8_t*>("\r\n"), 2);

    // Append body.
    if (!body.empty()) {
        response.append(body.data(), body.size());
    }

    send_raw(response);
}

void HTTPConnection::flush_write_buffer() {
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

void HTTPConnection::handle_send_completion(int result) {
    is_sending_ = false;

    if (result < 0) {
        set_state(ConnectionState::Error);
        if (error_handler_) {
            error_handler_(this, error{});
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

void HTTPConnection::close() {
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

void HTTPConnection::handle_read() {
    // Read up to 8KB per call.
    constexpr size_t kReadChunkSize = 8192;
    uint8_t* area = read_buffer_.reserve_tail(kReadChunkSize);
    ssize_t n = ::read(fd_, area, kReadChunkSize);
    if (n > 0) {
        read_buffer_.commit_tail(static_cast<size_t>(n));
        // Feed to parser.
        std::span<const uint8_t> data(read_buffer_.data(), read_buffer_.size());
        size_t consumed = parser_->execute(data);
        // Consume parsed bytes from read buffer.
        if (consumed > 0) {
            read_buffer_.consume(consumed);
        }
    } else if (n == 0) {
        // EOF — connection closed by peer.
        read_buffer_.clear();
        set_state(ConnectionState::Disconnected);
    } else {
        // n < 0: error.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        read_buffer_.clear();
    }
}

bool HTTPConnection::should_keep_alive() const {
    return parser_ && parser_->should_keep_alive();
}

} // namespace net
} // namespace hpactor
