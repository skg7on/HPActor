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

/// \brief TLS handshake protocol message types.
enum class TlsMessageType : uint8_t {
    ClientHello = 1,       ///< Client initiates handshake with nonce.
    ServerHello = 2,       ///< Server responds with nonce + encrypted PMS.
    Certificate = 3,       ///< Certificate exchange.
    CertificateVerify = 4, ///< Proof of private key possession.
    Finished = 5,          ///< Handshake complete confirmation.
};

/// \brief States of the TLS handshake state machine.
enum class TlsHandshakeState : uint8_t {
    Idle,                        ///< Handshake not yet started.
    WaitingForServerHello,       ///< Client: waiting for ServerHello.
    WaitingForCertificate,       ///< Waiting for peer certificate.
    WaitingForCertificateVerify, ///< Waiting for certificate verification.
    WaitingForFinished,          ///< Waiting for Finished message.
    HandshakeComplete,           ///< Handshake succeeded.
    Error,                       ///< Handshake failed.
};

/// \brief TLS session state (post-handshake).
enum class TlsSessionState : uint8_t {
    Handshake, ///< Handshake in progress; data is not yet encrypted.
    Encrypted, ///< Session key established; data is encrypted.
    Error,     ///< Session error; connection should be closed.
};

/// \brief Fixed-size nonce used in the TLS handshake.
constexpr size_t kNonceSize = 32;
using Nonce = std::array<uint8_t, kNonceSize>;

class TlsConnection;
using TlsConnectionPtr = std::shared_ptr<TlsConnection>;

/// \brief TLS connection with handshake and AES-256-CBC encrypted session.
///
/// Implements a TLS-like handshake protocol with mutual certificate
/// authentication and RSA key transport for session encryption.
/// Inherits from both \c Connection (for I/O) and
/// \c enable_shared_from_this (for safe callback capture).
///
/// \note Thread safety: Called from the event loop thread.
class TlsConnection : public Connection,
                      public std::enable_shared_from_this<TlsConnection> {
  public:
    /// \brief Create a client-side TLS connection (initiates handshake).
    ///
    /// \param[in] local_endpoint Local address.
    /// \param[in] remote_endpoint Server address.
    /// \param[in] tls_context TLS configuration and crypto.
    /// \param[in] loop Owning event loop.
    /// \return Shared pointer to the new connection.
    static TlsConnectionPtr
    create_client(EndPoint local_endpoint, EndPoint remote_endpoint,
                  TlsContext* tls_context, EventLoop* loop);

    /// \brief Create a server-side TLS connection from an accepted socket.
    ///
    /// \param[in] fd Accepted client file descriptor.
    /// \param[in] local_endpoint Server address.
    /// \param[in] remote_endpoint Client address.
    /// \param[in] tls_context TLS configuration and crypto.
    /// \param[in] loop Owning event loop.
    /// \return Shared pointer to the new connection.
    static TlsConnectionPtr
    create_server(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                  TlsContext* tls_context, EventLoop* loop);

    /// \brief Set the file descriptor (for connected client sockets after
    /// TCP handshake).
    ///
    /// \param[in] fd Connected socket file descriptor.
    void set_fd(int fd);

    ~TlsConnection();

    /// \name Non-copyable
    /// @{
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;
    /// @}

    /// \brief Set the connection-ready callback.
    ///
    /// \param[in] handler Invoked after handshake completes.
    void set_ready_handler(std::function<void(ConnectionPtr)> handler);

    /// \brief Set the frame handler for incoming decrypted frames.
    ///
    /// \param[in] handler Invoked for each complete decrypted frame.
    void set_frame_handler(frame_handler handler);

    /// \brief Set the error handler.
    ///
    /// \param[in] handler Invoked on handshake or encryption errors.
    void
    set_error_handler(std::function<void(ConnectionPtr, const error&)> handler);

    /// \brief Set the send-completion handler.
    ///
    /// \param[in] handler Invoked when an async send completes.
    void set_send_completion_handler(std::function<void(int result)> handler);

    /// \brief Complete post-connect setup.
    ///
    /// Registers for Read events and establishes the read handler.
    /// Static to avoid \c shared_from_this issues with dual
    /// \c enable_shared_from_this inheritance.
    /// \param[in] conn The connection to set up.
    static void setup_after_connect(TlsConnectionPtr conn);

    /// \brief Initiate the client-side TLS handshake.
    ///
    /// Called after TCP connection is established. Sends
    /// \c ClientHello.
    void start_client_handshake();

    /// \brief Handle incoming data from the socket.
    ///
    /// \note Thread safety: Called from the event loop thread.
    void handle_read() override;

    /// \brief Send an encrypted frame.
    ///
    /// \param[in] frame_data Plaintext frame data to encrypt and send.
    void send(const StreamBuffer& frame_data) override;

    /// \brief Close the connection.
    void close() override;

    /// \brief Handle async send completion.
    ///
    /// \param[in] result Byte count or negative errno.
    void handle_send_completion(int result) override;

    /// \brief Return the current session state.
    ///
    /// \return \c Handshake, \c Encrypted, or \c Error.
    TlsSessionState session_state() const {
        return session_state_;
    }

  private:
    TlsConnection(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                  TlsContext* tls_context, EventLoop* loop);

    // Handshake message builders
    StreamBuffer build_client_hello();
    StreamBuffer build_server_hello();
    StreamBuffer build_certificate();
    StreamBuffer build_certificate_verify(const Nonce& challenge);
    StreamBuffer build_finished();

    // Handshake message handlers
    void handle_client_hello(const StreamBuffer& data);
    void handle_server_hello(const StreamBuffer& data);
    void handle_certificate(const StreamBuffer& data);
    void handle_certificate_verify(const StreamBuffer& data);
    void handle_finished(const StreamBuffer& data);

    void process_buffer();
    void derive_session_keys(const StreamBuffer& pre_master_secret,
                             const Nonce& client_nonce, const Nonce& server_nonce);
    StreamBuffer encrypt_aes(const StreamBuffer& plaintext);
    StreamBuffer decrypt_aes(const StreamBuffer& ciphertext);
    StreamBuffer prf_sha256(const StreamBuffer& secret, const char* label,
                            const StreamBuffer& data);

    void set_handshake_state(TlsHandshakeState new_state);
    void set_session_state(TlsSessionState new_state);
    void send_raw(const StreamBuffer& data);
    void flush_write_buffer();

    TlsContext* tls_context_ = nullptr;
    TlsHandshakeState handshake_state_ = TlsHandshakeState::Idle;
    TlsSessionState session_state_ = TlsSessionState::Handshake;

    Nonce client_nonce_;
    Nonce server_nonce_;
    StreamBuffer pre_master_secret_;
    StreamBuffer encrypted_pms_;
    StreamBuffer master_secret_;
    StreamBuffer session_key_;
    StreamBuffer session_iv_;

    adt::StreamBuffer read_buffer_;
    adt::StreamBuffer write_buffer_;
    bool is_sending_ = false;
    StreamBuffer handshake_messages_;
    std::weak_ptr<TlsConnection> weak_self_;

    std::function<void(ConnectionPtr)> ready_handler_;
    frame_handler frame_handler_;
    std::function<void(ConnectionPtr, const error&)> error_handler_;
    std::function<void(int result)> send_completion_handler_;
    bool is_server_ = false;

    static constexpr size_t kReadChunkSize = 65536;
};

} // namespace net
} // namespace hpactor
