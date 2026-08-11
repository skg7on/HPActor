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

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/log/logger.hpp>
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

    if (conn->handshake_config_.enabled) {
        conn->set_state(ConnectionState::Handshake);
        HPACTOR_LOG_DEBUG(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "connection opened, starting handshake");
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
        conn->start_handshake();
    } else {
        conn->set_state(ConnectionState::Connected);
        HPACTOR_LOG_DEBUG(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "connection opened");
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
    if (conn->handshake_config_.enabled) {
        conn->set_state(ConnectionState::Handshake);
        HPACTOR_LOG_DEBUG(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "connection established, starting handshake");

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
        conn->start_handshake();
    } else {
        conn->set_state(ConnectionState::Connected);

        HPACTOR_LOG_DEBUG(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "connection opened");

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
}

WireFrameConnectionPtr
WireFrameConnection::create_as_server(int fd, EndPoint local_endpoint,
                                      EndPoint remote_endpoint, EventLoop* loop) {
    auto conn = std::shared_ptr<WireFrameConnection>(
        new WireFrameConnection(fd, local_endpoint, remote_endpoint, loop));

    if (conn->handshake_config_.enabled) {
        conn->set_state(ConnectionState::Handshake);
        HPACTOR_LOG_DEBUG(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "connection accepted, waiting for handshake");
    } else {
        conn->set_state(ConnectionState::Connected);
        HPACTOR_LOG_DEBUG(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "connection opened");
    }

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

void WireFrameConnection::set_max_inbound_frame_bytes(uint32_t max_bytes) {
    max_inbound_frame_bytes_ = max_bytes;
}

void WireFrameConnection::set_frame_error_handler(
    std::function<void(FrameDecodeError, uint32_t)> handler) {
    frame_error_handler_ = std::move(handler);
}

void WireFrameConnection::send(const StreamBuffer& frame_data) {
    if (state_ != ConnectionState::Connected &&
        state_ != ConnectionState::Handshake) {
        return;
    }

    HPACTOR_LOG_TRACE(
        log::LogCategory::kNetwork, ActorId{0}, 0, "network frame sent",
        log::field("bytes", static_cast<uint64_t>(frame_data.size())));

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
    HPACTOR_LOG_DEBUG(log::LogCategory::kNetwork, ActorId{0}, 0,
                      "connection closed");
}

void WireFrameConnection::handle_read() {
    // During the handshake phase, delegate to handshake-specific read logic.
    if (state_ == ConnectionState::Handshake) {
        handle_handshake_read();
        return;
    }

    FAULT_INJECT("hpactor.wireframe.handle_read.drop") {
        return;
    }

    // Iterative frame processing — bounded per event-loop turn.
    // Process at most 64 frames in one call to bound resync work.
    constexpr size_t kMaxFramesPerTurn = 64;
    size_t frames_processed = 0;

    while (frames_processed < kMaxFramesPerTurn) {
        // Phase 1 — accumulate header bytes (8 bytes: magic + payload length).
        if (read_buffer_.size() < WireFrame::HeaderSize) {
            size_t remaining = WireFrame::HeaderSize - read_buffer_.size();
            uint8_t* area = read_buffer_.reserve_tail(remaining);
            ssize_t n = ::read(fd_, area, remaining);
            if (n > 0) {
                read_buffer_.commit_tail(static_cast<size_t>(n));
                if (read_buffer_.size() < WireFrame::HeaderSize)
                    return; // Need more data
            } else if (n == 0) {
                read_buffer_.clear();
                set_state(ConnectionState::Disconnected);
                if (error_handler_) {
                    error_handler_(nullptr, error(errors::unknown, "EOF on read"));
                }
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                read_buffer_.clear();
                set_state(ConnectionState::Error);
                if (error_handler_) {
                    error_handler_(nullptr, error(errors::unknown, "read error"));
                }
                return;
            }
        }

        // Validate magic "HPAC" — iterative resync (no recursion).
        if (read_buffer_[0] != 'H' || read_buffer_[1] != 'P' ||
            read_buffer_[2] != 'A' || read_buffer_[3] != 'C') {
            read_buffer_.consume(1);
            if (frame_error_handler_) {
                frame_error_handler_(FrameDecodeError::InvalidMagic, 1);
            }
            continue; // Try next header position
        }

        // Parse payload length (big-endian uint32_t from bytes 4-7)
        size_t payload_len = (static_cast<size_t>(read_buffer_[4]) << 24) |
                             (static_cast<size_t>(read_buffer_[5]) << 16) |
                             (static_cast<size_t>(read_buffer_[6]) << 8) |
                             static_cast<size_t>(read_buffer_[7]);

        // Enforce inbound size bound before allocating
        if (max_inbound_frame_bytes_ > 0 && payload_len > max_inbound_frame_bytes_) {
            if (frame_error_handler_) {
                frame_error_handler_(FrameDecodeError::FrameTooLarge,
                                     static_cast<uint32_t>(payload_len));
            }
            read_buffer_.clear();
            close();
            return;
        }

        size_t total_frame_size = WireFrame::HeaderSize + payload_len;

        // Phase 2 — accumulate exactly payload_len bytes.
        while (read_buffer_.size() < total_frame_size) {
            size_t remaining = total_frame_size - read_buffer_.size();
            uint8_t* area = read_buffer_.reserve_tail(remaining);
            ssize_t n = ::read(fd_, area, remaining);
            if (n > 0) {
                read_buffer_.commit_tail(static_cast<size_t>(n));
            } else if (n == 0) {
                read_buffer_.clear();
                set_state(ConnectionState::Disconnected);
                if (error_handler_) {
                    error_handler_(nullptr, error(errors::unknown, "EOF mid-frame"));
                }
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                read_buffer_.clear();
                set_state(ConnectionState::Error);
                if (error_handler_) {
                    error_handler_(nullptr, error(errors::unknown, "read error"));
                }
                return;
            }
        }

        // Complete frame — deliver the canonical HPAC frame bytes
        // (header + payload) so plain and TLS paths present the same shape.
        StreamBuffer frame_data(read_buffer_.begin(),
                                read_buffer_.begin() + total_frame_size);
        read_buffer_.consume(total_frame_size);

        if (frame_handler_) {
            frame_handler_(std::move(frame_data));
        }

        ++frames_processed;
        // Continue loop if more data is buffered.
        if (read_buffer_.size() < WireFrame::HeaderSize)
            return;
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
    FAULT_INJECT("hpactor.wireframe.flush_write_buffer.drop") {
        return;
    }
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

// ── Protocol handshake (NET-001)
// ──────────────────────────────────────────────

void WireFrameConnection::start_handshake() {
    // Client path: send HandshakeHello.
    // Server path: just wait for the client to send hello first (no-op here).
    // We determine client vs server by checking if we have a pending hello to
    // send.  create_as_client() and setup_after_connect() call
    // start_handshake() on the connecting side, so we send hello.
    // create_as_server() does NOT call start_handshake() — it just sets state
    // to Handshake and waits.
    send_handshake_hello();
}

void WireFrameConnection::handle_handshake_read() {
    // Accumulate handshake header bytes (8 bytes: magic + payload length).
    if (read_buffer_.size() < HandshakeHeaderSize) {
        size_t remaining = HandshakeHeaderSize - read_buffer_.size();
        uint8_t* area = read_buffer_.reserve_tail(remaining);
        ssize_t n = ::read(fd_, area, remaining);
        if (n > 0) {
            read_buffer_.commit_tail(static_cast<size_t>(n));
            if (read_buffer_.size() < HandshakeHeaderSize)
                return; // Need more data
        } else if (n == 0) {
            // EOF during handshake — peer disconnected
            set_state(ConnectionState::Error);
            if (error_handler_) {
                error_handler_(nullptr,
                               error(errors::unknown, "EOF during handshake"));
            }
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            set_state(ConnectionState::Error);
            if (error_handler_) {
                error_handler_(nullptr, error(errors::unknown,
                                              "read error during handshake"));
            }
            return;
        }
    }

    // Validate magic "HPAH"
    if (read_buffer_[0] != 'H' || read_buffer_[1] != 'P' ||
        read_buffer_[2] != 'A' || read_buffer_[3] != 'H') {
        // Not a handshake message — this could be a legacy peer sending
        // "HPAC" frames directly. Treat as handshake failure.
        read_buffer_.clear();
        set_state(ConnectionState::Error);
        HPACTOR_LOG_WARNING(log::LogCategory::kNetwork, ActorId{0}, 0,
                            "handshake: invalid magic from peer");
        if (error_handler_) {
            error_handler_(nullptr,
                           error(errors::unknown, "invalid handshake magic"));
        }
        return;
    }

    // Parse payload length (big-endian uint32_t from bytes 4-7)
    size_t payload_len = (static_cast<size_t>(read_buffer_[4]) << 24) |
                         (static_cast<size_t>(read_buffer_[5]) << 16) |
                         (static_cast<size_t>(read_buffer_[6]) << 8) |
                         static_cast<size_t>(read_buffer_[7]);

    // Bound check
    if (payload_len > 65536) { // 64 KiB max for handshake messages
        read_buffer_.clear();
        set_state(ConnectionState::Error);
        if (error_handler_) {
            error_handler_(nullptr,
                           error(errors::unknown, "handshake payload too large"));
        }
        return;
    }

    size_t total_size = HandshakeHeaderSize + payload_len;

    // Accumulate payload bytes
    while (read_buffer_.size() < total_size) {
        size_t remaining = total_size - read_buffer_.size();
        uint8_t* area = read_buffer_.reserve_tail(remaining);
        ssize_t n = ::read(fd_, area, remaining);
        if (n > 0) {
            read_buffer_.commit_tail(static_cast<size_t>(n));
        } else if (n == 0) {
            set_state(ConnectionState::Error);
            if (error_handler_) {
                error_handler_(nullptr, error(errors::unknown, "EOF mid-handshake"));
            }
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            set_state(ConnectionState::Error);
            if (error_handler_) {
                error_handler_(nullptr, error(errors::unknown,
                                              "read error mid-handshake"));
            }
            return;
        }
    }

    // Complete handshake message received — copy and consume from read buffer
    StreamBuffer msg_data(read_buffer_.begin(), read_buffer_.begin() + total_size);
    read_buffer_.consume(total_size);

    // Determine which message we received based on our role
    if (!handshake_hello_sent_) {
        // Server path: we haven't sent hello yet, so this must be a
        // HandshakeHello from the client.
        auto hello = decode_handshake_hello(msg_data);
        if (!hello.has_value()) {
            set_state(ConnectionState::Error);
            HPACTOR_LOG_WARNING(log::LogCategory::kNetwork, ActorId{0}, 0,
                                "handshake: failed to decode HandshakeHello");
            if (error_handler_) {
                error_handler_(nullptr,
                               error(errors::unknown, "handshake decode failed"));
            }
            return;
        }
        send_handshake_response(*hello);
    } else {
        // Client path: we already sent hello, this is the response.
        auto resp = decode_handshake_response(msg_data);
        if (!resp.has_value()) {
            set_state(ConnectionState::Error);
            HPACTOR_LOG_WARNING(log::LogCategory::kNetwork, ActorId{0}, 0,
                                "handshake: failed to decode HandshakeResponse");
            if (error_handler_) {
                error_handler_(nullptr,
                               error(errors::unknown, "handshake decode failed"));
            }
            return;
        }

        if (resp->result() != ::hpactor::net::HANDSHAKE_ACCEPTED) {
            // Peer rejected the handshake
            HPACTOR_LOG_WARNING(
                log::LogCategory::kNetwork, ActorId{0}, 0,
                "handshake rejected by peer",
                log::field("result", static_cast<uint64_t>(resp->result())));
            set_state(ConnectionState::Error);
            if (error_handler_) {
                error_handler_(nullptr, error(errors::unknown,
                                              "handshake rejected by peer"));
            }
            return;
        }

        // Negotiation successful — store agreed values
        handshake_config_.version_min = resp->accepted_version();
        handshake_config_.version_max = resp->accepted_version();
        handshake_config_.feature_flags = resp->feature_flags();
        complete_handshake();
    }
}

void WireFrameConnection::send_handshake_hello() {
    ::hpactor::net::HandshakeHello hello;
    hello.set_min_version(handshake_config_.version_min);
    hello.set_max_version(handshake_config_.version_max);
    hello.set_feature_flags(handshake_config_.feature_flags);
    hello.set_node_id(handshake_config_.node_id);

    StreamBuffer encoded = encode_handshake_hello(hello);
    if (encoded.empty()) {
        set_state(ConnectionState::Error);
        HPACTOR_LOG_ERROR(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "handshake: failed to encode HandshakeHello");
        if (error_handler_) {
            error_handler_(nullptr,
                           error(errors::unknown, "handshake encode failed"));
        }
        return;
    }

    handshake_hello_sent_ = true;
    send_raw(encoded);
}

void WireFrameConnection::send_handshake_response(
    const ::hpactor::net::HandshakeHello& hello) {
    uint32_t accepted = negotiate_version(hello.min_version(), hello.max_version(),
                                          handshake_config_.version_min,
                                          handshake_config_.version_max);

    ::hpactor::net::HandshakeResponse resp;
    resp.set_node_id(handshake_config_.node_id);

    if (accepted == 0) {
        // Version ranges are disjoint
        resp.set_accepted_version(0);
        resp.set_feature_flags(0);
        resp.set_result(::hpactor::net::HANDSHAKE_INCOMPATIBLE_VERSION);

        HPACTOR_LOG_WARNING(
            log::LogCategory::kNetwork, ActorId{0}, 0,
            "handshake: incompatible version",
            log::field("client_min", static_cast<uint64_t>(hello.min_version())),
            log::field("client_max", static_cast<uint64_t>(hello.max_version())),
            log::field("server_min",
                       static_cast<uint64_t>(handshake_config_.version_min)),
            log::field("server_max",
                       static_cast<uint64_t>(handshake_config_.version_max)));
    } else {
        // Negotiate feature flags
        uint64_t agreed_flags =
            hello.feature_flags() & handshake_config_.feature_flags;

        resp.set_accepted_version(accepted);
        resp.set_feature_flags(agreed_flags);
        resp.set_result(::hpactor::net::HANDSHAKE_ACCEPTED);

        // Store negotiated values
        handshake_config_.version_min = accepted;
        handshake_config_.version_max = accepted;
        handshake_config_.feature_flags = agreed_flags;
    }

    StreamBuffer encoded = encode_handshake_response(resp);
    if (encoded.empty()) {
        set_state(ConnectionState::Error);
        HPACTOR_LOG_ERROR(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "handshake: failed to encode HandshakeResponse");
        if (error_handler_) {
            error_handler_(nullptr,
                           error(errors::unknown, "handshake encode failed"));
        }
        return;
    }

    send_raw(encoded);

    if (resp.result() == ::hpactor::net::HANDSHAKE_ACCEPTED) {
        // Response sent, handshake complete (server side)
        complete_handshake();
    } else {
        // Rejection sent — close connection after the response is flushed
        set_state(ConnectionState::Error);
        if (error_handler_) {
            error_handler_(nullptr,
                           error(errors::unknown,
                                 "handshake rejected: incompatible version"));
        }
    }
}

void WireFrameConnection::complete_handshake() {
    handshake_complete_ = true;
    set_state(ConnectionState::Connected);

    HPACTOR_LOG_DEBUG(
        log::LogCategory::kNetwork, ActorId{0}, 0, "handshake complete",
        log::field("version", static_cast<uint64_t>(handshake_config_.version_max)),
        log::field("features",
                   static_cast<uint64_t>(handshake_config_.feature_flags)));

    if (ready_handler_) {
        ready_handler_(
            std::enable_shared_from_this<WireFrameConnection>::shared_from_this());
    }
}

} // namespace net
} // namespace hpactor
