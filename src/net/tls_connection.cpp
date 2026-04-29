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

#include <hpactor/net/tls_connection.hpp>

#include <hpactor/net/event_loop.hpp>

#include <cstring>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hpactor {

namespace net {

namespace {

// Format a TLS message with type and payload
bytes format_tls_message(TlsMessageType type, const bytes& payload) {
    bytes msg;
    msg.push_back(static_cast<uint8_t>(type));
    // Add length as 3 bytes (TLS style)
    msg.push_back(static_cast<uint8_t>((payload.size() >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}

// Parse a TLS message - extract payload from formatted message
bytes parse_tls_payload(const uint8_t* data, size_t len, size_t& consumed) {
    consumed = 0;
    if (len < 4) {
        return bytes{};
    }
    size_t payload_len = (static_cast<size_t>(data[1]) << 16) |
                         (static_cast<size_t>(data[2]) << 8) |
                         static_cast<size_t>(data[3]);
    size_t total_len = 4 + payload_len;
    if (len < total_len) {
        return bytes{};
    }
    bytes payload(data + 4, data + total_len);
    consumed = total_len;
    return payload;
}

} // anonymous namespace

TlsConnection::TlsConnection(CommunicationEndpoint remote_endpoint,
                             TlsContext* tls_context, EventLoop* loop, int socket_fd)
    : Connection(remote_endpoint), remote_endpoint_(remote_endpoint),
      tls_context_(tls_context), loop_(loop), fd_(socket_fd) {
    // Generate random client nonce
    RAND_bytes(client_nonce_.data(), static_cast<int>(kNonceSize));
}

TlsConnection::~TlsConnection() {
    close();
}

TlsConnectionPtr
TlsConnection::create_client(CommunicationEndpoint remote_endpoint,
                             TlsContext* tls_context, EventLoop* loop) {
    auto conn = std::shared_ptr<TlsConnection>(
        new TlsConnection(remote_endpoint, tls_context, loop, -1));
    conn->set_state(ConnectionState::Connecting);
    conn->is_server_ = false;
    return conn;
}

TlsConnectionPtr
TlsConnection::create_server(int socket_fd, CommunicationEndpoint remote_endpoint,
                             TlsContext* tls_context, EventLoop* loop) {
    auto conn = std::shared_ptr<TlsConnection>(
        new TlsConnection(remote_endpoint, tls_context, loop, socket_fd));
    conn->set_state(ConnectionState::Connected);
    conn->is_server_ = true;
    // Server waits for client hello
    conn->set_handshake_state(TlsHandshakeState::WaitingForServerHello);
    conn->set_session_state(TlsSessionState::Handshake);

    // Register fd with event loop for read events (for receiving handshake
    // messages)
    if (loop && socket_fd >= 0) {
        loop->add_fd(socket_fd, EventLoop::Event::Read);
    }

    return conn;
}

void TlsConnection::set_ready_handler(std::function<void(ConnectionPtr)> handler) {
    ready_handler_ = std::move(handler);
}

void TlsConnection::set_frame_handler(frame_handler handler) {
    frame_handler_ = std::move(handler);
}

void TlsConnection::set_error_handler(
    std::function<void(ConnectionPtr, const error&)> handler) {
    error_handler_ = std::move(handler);
}

void TlsConnection::set_send_completion_handler(std::function<void(int result)> handler) {
    send_completion_handler_ = std::move(handler);
}

void TlsConnection::set_fd(int fd) {
    fd_ = fd;
    // Register fd with event loop for read events
    if (loop_ && fd >= 0) {
        loop_->add_fd(fd, EventLoop::Event::Read);
    }
}

void TlsConnection::start_client_handshake() {
    if (is_server_)
        return;

    // Generate client nonce if not already done
    bool all_zero = true;
    for (auto b : client_nonce_) {
        if (b != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        RAND_bytes(client_nonce_.data(), static_cast<int>(kNonceSize));
    }

    set_handshake_state(TlsHandshakeState::WaitingForServerHello);
    set_state(ConnectionState::Handshake);

    bytes client_hello = build_client_hello();
    send_raw(client_hello);
}

void TlsConnection::handle_read(const bytes& data) {
    read_buffer_.append(data.data(), data.size());

    // Process complete messages in buffer
    while (read_buffer_.size() >= 4) {
        size_t consumed = 0;
        bytes payload = parse_tls_payload(read_buffer_.data(),
                                          read_buffer_.size(), consumed);
        if (consumed == 0) {
            break; // Wait for more data
        }
        read_buffer_.consume(consumed);

        if (session_state_ == TlsSessionState::Encrypted) {
            // Decrypt and deliver to frame handler
            bytes plaintext = decrypt_aes(payload);
            if (!plaintext.empty() && frame_handler_) {
                frame_handler_(plaintext);
            }
        } else {
            // Handle handshake messages
            TlsMessageType msg_type = static_cast<TlsMessageType>(payload[0]);
            bytes msg_payload(payload.begin() + 1, payload.end());

            switch (msg_type) {
                case TlsMessageType::ServerHello:
                    handle_server_hello(msg_payload);
                    break;
                case TlsMessageType::Certificate:
                    handle_certificate(msg_payload);
                    break;
                case TlsMessageType::CertificateVerify:
                    handle_certificate_verify(msg_payload);
                    break;
                case TlsMessageType::Finished:
                    handle_finished(msg_payload);
                    break;
                default:
                    set_handshake_state(TlsHandshakeState::Error);
                    break;
            }
        }

        // Check if we should stop processing
        if (handshake_state_ == TlsHandshakeState::Error ||
            session_state_ == TlsSessionState::Error) {
            break;
        }
    }
}

void TlsConnection::send(const bytes& frame_data) {
    if (session_state_ == TlsSessionState::Encrypted) {
        bytes encrypted = encrypt_aes(frame_data);
        send_raw(format_tls_message(TlsMessageType::Finished, encrypted));
    }
}

void TlsConnection::close() {
    if (fd_ >= 0) {
        if (loop_) {
            loop_->remove_fd(fd_);
        }
        ::close(fd_);
        fd_ = -1;
    }
    set_state(ConnectionState::Disconnected);
}

bytes TlsConnection::build_client_hello() {
    bytes payload;
    // Message type
    payload.push_back(static_cast<uint8_t>(TlsMessageType::ClientHello));
    // Client nonce (32 bytes)
    payload.insert(payload.end(), client_nonce_.begin(), client_nonce_.end());
    // Public key from TLS context
    if (!tls_context_) {
        set_handshake_state(TlsHandshakeState::Error);
        return bytes{};
    }
    const bytes& pub_key = tls_context_->public_key();
    payload.insert(payload.end(), pub_key.begin(), pub_key.end());

    bytes msg = format_tls_message(TlsMessageType::ClientHello, payload);
    handshake_messages_.insert(handshake_messages_.end(), msg.begin(), msg.end());
    return msg;
}

bytes TlsConnection::build_certificate() {
    bytes payload;
    // Certificate data from TLS context
    if (!tls_context_) {
        set_handshake_state(TlsHandshakeState::Error);
        return bytes{};
    }
    const bytes& cert = tls_context_->certificate();
    payload.insert(payload.end(), cert.begin(), cert.end());

    bytes msg = format_tls_message(TlsMessageType::Certificate, payload);
    handshake_messages_.insert(handshake_messages_.end(), msg.begin(), msg.end());
    return msg;
}

bytes TlsConnection::build_certificate_verify(const Nonce& challenge) {
    bytes payload;
    // Sign the challenge nonce with our private key
    bytes data_to_sign(challenge.begin(), challenge.end());
    if (!tls_context_) {
        set_handshake_state(TlsHandshakeState::Error);
        return bytes{};
    }
    bytes signature = tls_context_->sign_data(data_to_sign);
    payload.insert(payload.end(), signature.begin(), signature.end());

    bytes msg = format_tls_message(TlsMessageType::CertificateVerify, payload);
    handshake_messages_.insert(handshake_messages_.end(), msg.begin(), msg.end());
    return msg;
}

bytes TlsConnection::build_finished() {
    bytes payload;
    // Compute verify_data using PRF
    bytes verify_data = prf_sha256(master_secret_, "finished", handshake_messages_);
    payload.insert(payload.end(), verify_data.begin(), verify_data.end());

    return format_tls_message(TlsMessageType::Finished, payload);
}

void TlsConnection::handle_server_hello(const bytes& data) {
    if (handshake_state_ != TlsHandshakeState::WaitingForServerHello) {
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    if (data.size() < kNonceSize) {
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    // Extract server nonce
    std::memcpy(server_nonce_.data(), data.data(), kNonceSize);

    // Note: Server's public key would be extracted from its certificate
    // for encrypting pre_master_secret. This is handled in a full TLS
    // implementation by the certificate message that follows.

    // Send our certificate
    bytes cert_msg = build_certificate();
    send_raw(cert_msg);

    // Generate pre_master_secret
    pre_master_secret_.resize(48);
    RAND_bytes(pre_master_secret_.data(), 48);

    set_handshake_state(TlsHandshakeState::WaitingForCertificate);
}

void TlsConnection::handle_certificate(const bytes& data) {
    if (handshake_state_ != TlsHandshakeState::WaitingForCertificate) {
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    if (!tls_context_) {
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    // Verify the certificate
    auto result = tls_context_->verify_certificate(data);
    if (result != TlsContext::CertVerifyResult::Ok) {
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    // Send certificate verify message
    bytes verify_msg = build_certificate_verify(server_nonce_);
    send_raw(verify_msg);

    set_handshake_state(TlsHandshakeState::WaitingForCertificateVerify);
}

void TlsConnection::handle_certificate_verify(const bytes& data) {
    (void)data;
    if (handshake_state_ != TlsHandshakeState::WaitingForCertificateVerify) {
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    // Derive session keys
    derive_session_keys(pre_master_secret_, client_nonce_, server_nonce_);

    set_handshake_state(TlsHandshakeState::WaitingForFinished);
}

void TlsConnection::handle_finished(const bytes& data) {
    (void)data;
    if (handshake_state_ != TlsHandshakeState::WaitingForFinished) {
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    // Verify the finished message
    // In a full implementation, we would verify the verify_data here

    set_handshake_state(TlsHandshakeState::HandshakeComplete);
    set_session_state(TlsSessionState::Encrypted);
    set_state(ConnectionState::Connected);

    // Notify ready handler
    if (ready_handler_) {
        TlsConnectionPtr self =
            std::enable_shared_from_this<TlsConnection>::shared_from_this();
        ready_handler_(self);
    }
}

void TlsConnection::derive_session_keys(const bytes& pre_master_secret,
                                        const Nonce& client_nonce,
                                        const Nonce& server_nonce) {
    bytes random_data;
    random_data.insert(random_data.end(), client_nonce.begin(), client_nonce.end());
    random_data.insert(random_data.end(), server_nonce.begin(), server_nonce.end());
    master_secret_ = prf_sha256(pre_master_secret, "master secret", random_data);

    random_data.clear();
    random_data.insert(random_data.end(), server_nonce.begin(), server_nonce.end());
    random_data.insert(random_data.end(), client_nonce.begin(), client_nonce.end());
    bytes key_block = prf_sha256(master_secret_, "key expansion", random_data);

    session_key_.assign(key_block.begin(), key_block.begin() + 32);
    session_iv_.assign(key_block.begin() + 32, key_block.begin() + 48);
}

namespace {

// HMAC-SHA256 using EVP_Q_mac (OpenSSL 3.0 compatible)
bytes hmac_sha256(const bytes& key, const bytes& data) {
    constexpr size_t hash_size = 32; // SHA256 output size
    bytes out(hash_size, 0);
    size_t out_len = hash_size;

    // Use EVP_Q_mac with HMAC algorithm and SHA256 digest
    EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(),
              data.data(), data.size(), out.data(), out.size(), &out_len);

    out.resize(out_len);
    return out;
}

} // anonymous namespace

bytes TlsConnection::prf_sha256(const bytes& secret, const char* label,
                                const bytes& data) {
    bytes result;
    bytes label_seed;
    label_seed.insert(label_seed.end(), label, label + std::strlen(label));
    label_seed.insert(label_seed.end(), data.begin(), data.end());

    bytes a = label_seed;

    while (result.size() < 48) { // Generate enough for master_secret + key
                                 // expansion
        // A(i) = HMAC(secret, A(i-1))
        a = hmac_sha256(secret, a);

        // HMAC(secret, A(i) + label_seed)
        bytes a_label_seed = a;
        a_label_seed.insert(a_label_seed.end(), label_seed.begin(),
                            label_seed.end());
        bytes h = hmac_sha256(secret, a_label_seed);

        result.insert(result.end(), h.begin(), h.end());
    }
    return result;
}

bytes TlsConnection::encrypt_aes(const bytes& plaintext) {
    bytes ciphertext;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return ciphertext;

    unsigned char iv[16];
    std::memcpy(iv, session_iv_.data(), 16);

    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, session_key_.data(), iv);
    int len = 0;
    ciphertext.resize(plaintext.size() + AES_BLOCK_SIZE);
    EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(),
                      static_cast<int>(plaintext.size()));
    int ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len);
    ciphertext_len += len;
    ciphertext.resize(static_cast<size_t>(ciphertext_len));

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext;
}

bytes TlsConnection::decrypt_aes(const bytes& ciphertext) {
    bytes plaintext;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return plaintext;

    unsigned char iv[16];
    std::memcpy(iv, session_iv_.data(), 16);

    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, session_key_.data(), iv);
    int len = 0;
    plaintext.resize(ciphertext.size());
    EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(),
                      static_cast<int>(ciphertext.size()));
    int plaintext_len = len;
    EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
    plaintext_len += len;
    plaintext.resize(static_cast<size_t>(plaintext_len));

    EVP_CIPHER_CTX_free(ctx);
    return plaintext;
}

void TlsConnection::set_state(ConnectionState new_state) {
    state_ = new_state;
}

void TlsConnection::set_handshake_state(TlsHandshakeState new_state) {
    handshake_state_ = new_state;
    if (new_state == TlsHandshakeState::Error) {
        session_state_ = TlsSessionState::Error;
        set_state(ConnectionState::Error);
    }
}

void TlsConnection::set_session_state(TlsSessionState new_state) {
    session_state_ = new_state;
}

void TlsConnection::send_raw(const bytes& data) {
    if (fd_ < 0 || !loop_)
        return;

    // Append data to write buffer
    write_buffer_.append(data.data(), data.size());

    // If already sending, wait for completion
    if (is_sending_)
        return;

    flush_write_buffer();
}

void TlsConnection::flush_write_buffer() {
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

void TlsConnection::handle_send_completion(int result) {
    if (send_completion_handler_) {
        send_completion_handler_(result);
    }
    is_sending_ = false;

    if (result < 0) {
        // Send error - close connection
        set_handshake_state(TlsHandshakeState::Error);
        return;
    }

    // Remove sent bytes from write buffer
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

void TlsConnection::on_fd_readable() {
    // Would be called by EventLoop when fd is readable
    // Read data and call handle_read
}

void TlsConnection::on_fd_writable() {
    // Would be called by EventLoop when fd is writable
    // Flush write buffer
}

} // namespace net
} // namespace hpactor
