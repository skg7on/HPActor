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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/tls_context.hpp>
#include <hpactor/net/transport.hpp>

#include <array>
#include <deque>
#include <functional>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// TlsConnection - TLS connection with handshake and encrypted session
// -----------------------------------------------------------------------------
// Implements the TLS-like handshake protocol with mutual certificate
// authentication and RSA key transport for session encryption.
// -----------------------------------------------------------------------------

// TLS message types for the handshake protocol
enum class TlsMessageType : uint8_t {
    ClientHello = 1,
    ServerHello = 2,
    Certificate = 3,
    CertificateVerify = 4,
    Finished = 5,
};

// Handshake state machine states
enum class TlsHandshakeState : uint8_t {
    Idle,
    WaitingForServerHello,
    WaitingForCertificate,
    WaitingForCertificateVerify,
    WaitingForFinished,
    HandshakeComplete,
    Error,
};

// Session state (post-handshake)
enum class TlsSessionState : uint8_t {
    Handshake,
    Encrypted,
    Error,
};

// Fixed-size nonces
constexpr size_t kNonceSize = 32;
using Nonce = std::array<uint8_t, kNonceSize>;

// Connection pointer
class TlsConnection;
using TlsConnectionPtr = std::shared_ptr<TlsConnection>;

class TlsConnection : public Connection,
                      public std::enable_shared_from_this<TlsConnection> {
  public:
    // Create client-side connection
    static TlsConnectionPtr create_client(EndPoint remote_endpoint,
                                          TlsContext* tls_context, EventLoop* loop);

    // Create server-side connection (from accepted socket)
    static TlsConnectionPtr
    create_server(int fd, EndPoint remote_endpoint,
                  TlsContext* tls_context, EventLoop* loop);

    // Set file descriptor (for connected client sockets after TCP handshake)
    void set_fd(int fd);

    ~TlsConnection();

    // Non-copyable
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    // Getters
    EndPoint remote_endpoint() const {
        return remote_endpoint_;
    }
    ConnectionState state() const {
        return state_;
    }
    int fd() const {
        return fd_;
    }

    // Set callbacks
    void set_ready_handler(std::function<void(ConnectionPtr)> handler);
    void set_frame_handler(frame_handler handler);
    void
    set_error_handler(std::function<void(ConnectionPtr, const error&)> handler);
    void set_send_completion_handler(std::function<void(int result)> handler);

    // Initiate client handshake (called after connection established)
    void start_client_handshake();

    // Handle incoming data from socket
    void handle_read(const bytes& data);

    // Send encrypted frame
    void send(const bytes& frame_data) override;

    // Close connection
    void close() override;

    // Handle send completion (called by TcpTransport on async_send completion)
    void handle_send_completion(int result) override;

    // Get session state
    TlsSessionState session_state() const {
        return session_state_;
    }

  private:
    TlsConnection(EndPoint remote_endpoint,
                  TlsContext* tls_context, EventLoop* loop, int fd = -1);

    // Handshake message builders
    bytes build_client_hello();
    bytes build_certificate();
    bytes build_certificate_verify(const Nonce& challenge);
    bytes build_finished();

    // Handshake message handlers
    void handle_server_hello(const bytes& data);
    void handle_certificate(const bytes& data);
    void handle_certificate_verify(const bytes& data);
    void handle_finished(const bytes& data);

    // Helper: derive session key from pre_master_secret
    void derive_session_keys(const bytes& pre_master_secret,
                             const Nonce& client_nonce, const Nonce& server_nonce);

    // Helper: encrypt data with session key (AES-256-CBC)
    bytes encrypt_aes(const bytes& plaintext);

    // Helper: decrypt data with session key
    bytes decrypt_aes(const bytes& ciphertext);

    // Helper: compute TLS PRF (SHA-256 based)
    bytes prf_sha256(const bytes& secret, const char* label, const bytes& data);

    // Transition state
    void set_state(ConnectionState new_state);
    void set_handshake_state(TlsHandshakeState new_state);
    void set_session_state(TlsSessionState new_state);

    // Send raw bytes on socket
    void send_raw(const bytes& data);

    // Flush write buffer (called after async_send completion)
    void flush_write_buffer();

    // Event loop callbacks
    void on_fd_readable();
    void on_fd_writable();

    EndPoint remote_endpoint_ = LocalEndpoint;
    TlsContext* tls_context_ = nullptr;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;

    ConnectionState state_ = ConnectionState::Disconnected;
    TlsHandshakeState handshake_state_ = TlsHandshakeState::Idle;
    TlsSessionState session_state_ = TlsSessionState::Handshake;

    // Handshake nonces
    Nonce client_nonce_;
    Nonce server_nonce_;
    bytes pre_master_secret_;

    // Session keys
    bytes master_secret_;
    bytes session_key_; // AES-256 key
    bytes session_iv_;  // AES IV

    // Read buffer
    adt::StreamBuffer read_buffer_;

    // Write buffer
    adt::StreamBuffer write_buffer_;

    // True while async send is in progress
    bool is_sending_ = false;

    // Handshake message buffer (for Finished verify_data)
    bytes handshake_messages_;

    // Callbacks
    std::function<void(ConnectionPtr)> ready_handler_;
    frame_handler frame_handler_;
    std::function<void(ConnectionPtr, const error&)> error_handler_;
    std::function<void(int result)> send_completion_handler_;

    // Server-side flag
    bool is_server_ = false;
};

} // namespace net
} // namespace hpactor
