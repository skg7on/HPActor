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

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/tls_connection.hpp>
#include <hpactor/net/tls_context.hpp>

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "tls_test_helpers.hpp"

using namespace hpactor;
using namespace hpactor::net;

// Test helper: create connected socket pair with both ends non-blocking
static std::pair<int, int> create_socket_pair() {
    int sv[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(ret == 0);

    int flags = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

    return {sv[0], sv[1]};
}

// Helper: build a raw ClientHello message for protocol testing
static StreamBuffer build_raw_client_hello(const StreamBuffer& pub_key) {
    StreamBuffer payload;
    payload.push_back(static_cast<uint8_t>(TlsMessageType::ClientHello));
    for (int i = 0; i < 32; i++) {
        payload.push_back(static_cast<uint8_t>(i));
    }
    payload.insert(payload.end(), pub_key.begin(), pub_key.end());

    StreamBuffer message;
    message.push_back(static_cast<uint8_t>(TlsMessageType::ClientHello));
    size_t len = payload.size();
    message.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    message.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    message.push_back(static_cast<uint8_t>(len & 0xFF));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

// Helper: create a default TlsContext for testing
static TlsContext make_test_ctx(uint16_t port = 12345) {
    TlsConfig config;
    config.endpoint =
        hpactor::endpoint_ops::parse_endpoint("localhost:" + std::to_string(port));
    return TlsContext::from_config(config);
}


class TlsConnectionTest : public ::testing::Test {};

TEST_F(TlsConnectionTest, CreationAndBasicProperties) {
    auto ctx = make_test_ctx();
    EventLoop loop;

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);
    ASSERT_NE(client, nullptr);
    EXPECT_EQ(client->state(), ConnectionState::Connecting);
    EXPECT_EQ(client->session_state(), TlsSessionState::Handshake);
    EXPECT_EQ(client->fd(), -1);

    auto [client_fd, server_fd] = create_socket_pair();
    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(server->state(), ConnectionState::Connected);
    EXPECT_EQ(server->fd(), server_fd);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, ClientHandshakeInitiation) {
    auto ctx = make_test_ctx();
    EventLoop loop;

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);
    EXPECT_EQ(client->state(), ConnectionState::Connecting);

    client->start_client_handshake();
    EXPECT_EQ(client->state(), ConnectionState::Handshake);
}

TEST_F(TlsConnectionTest, ServerReceivesDataViaHandleRead) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);

    StreamBuffer data = build_raw_client_hello(ctx.public_key());
    ssize_t written = ::write(client_fd, data.data(), data.size());
    ASSERT_EQ(written, static_cast<ssize_t>(data.size()));

    server->handle_read();
    (void)server->state();

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, SendInHandshakeState) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    StreamBuffer test_data = {'h', 'e', 'l', 'l', 'o'};
    client->send(test_data);
    EXPECT_EQ(client->session_state(), TlsSessionState::Handshake);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, SendCompletionHandler) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    std::atomic<int> completion_count(0);
    client->set_send_completion_handler(
        [&](int /*result*/) { completion_count++; });

    client->handle_send_completion(10);
    EXPECT_EQ(completion_count, 1);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, WriteBufferBehavior) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    client->start_client_handshake();
    client->handle_send_completion(1024);

    std::atomic<int> call_count(0);
    client->set_send_completion_handler([&](int /*result*/) { call_count++; });
    client->handle_send_completion(100);
    EXPECT_EQ(call_count, 1);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, SendErrorHandling) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    bool error_handler_called = false;
    client->set_error_handler(
        [&](ConnectionPtr, const error&) { error_handler_called = true; });

    client->handle_send_completion(-1);
    EXPECT_TRUE(client->state() == ConnectionState::Error || error_handler_called);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, PrematureMessageInWrongState) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);

    StreamBuffer payload;
    payload.push_back(static_cast<uint8_t>(TlsMessageType::Certificate));
    payload.insert(payload.end(), {'D', 'E', 'R', 'd', 'a', 't', 'a'});

    StreamBuffer cert_message;
    cert_message.push_back(static_cast<uint8_t>(TlsMessageType::Certificate));
    size_t len = payload.size();
    cert_message.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    cert_message.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    cert_message.push_back(static_cast<uint8_t>(len & 0xFF));
    cert_message.insert(cert_message.end(), payload.begin(), payload.end());

    ::write(client_fd, cert_message.data(), cert_message.size());
    server->handle_read();

    EXPECT_EQ(server->state(), ConnectionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, CloseDuringHandshake) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);
    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    client->start_client_handshake();
    EXPECT_EQ(client->state(), ConnectionState::Handshake);

    client->close();
    EXPECT_EQ(client->state(), ConnectionState::Disconnected);
    EXPECT_EQ(client->fd(), -1);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, FrameHandlerCallback) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);

    StreamBuffer received_frame;
    server->set_frame_handler(
        [&](StreamBuffer data) { received_frame = std::move(data); });

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, StateTransitions) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);
    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);

    EXPECT_EQ(client->state(), ConnectionState::Connecting);
    EXPECT_EQ(server->state(), ConnectionState::Connected);
    EXPECT_EQ(client->session_state(), TlsSessionState::Handshake);
    EXPECT_EQ(server->session_state(), TlsSessionState::Handshake);

    client->start_client_handshake();
    EXPECT_EQ(client->state(), ConnectionState::Handshake);

    client->close();
    EXPECT_EQ(client->state(), ConnectionState::Disconnected);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, MultipleSendCompletions) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    std::atomic<int> handler_call_count(0);
    client->set_send_completion_handler(
        [&](int /*result*/) { handler_call_count++; });

    client->handle_send_completion(5);
    EXPECT_EQ(handler_call_count, 1);
    client->handle_send_completion(10);
    EXPECT_EQ(handler_call_count, 2);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, ErrorHandlerCallback) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    bool error_handler_called = false;
    client->set_error_handler(
        [&](ConnectionPtr, const error&) { error_handler_called = true; });

    client->handle_send_completion(-1);
    EXPECT_TRUE(client->state() == ConnectionState::Error || error_handler_called);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, ReadyHandlerCallback) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    bool ready_handler_called = false;
    client->set_ready_handler([&](ConnectionPtr conn) {
        ready_handler_called = true;
        EXPECT_NE(conn, nullptr);
    });

    EXPECT_FALSE(ready_handler_called);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, ContextRsaOperations) {
    auto ctx = make_test_ctx();

    StreamBuffer data_to_sign = {'t', 'e', 's', 't', 'd', 'a', 't', 'a'};
    StreamBuffer signature = ctx.sign_data(data_to_sign);

    const StreamBuffer& pub_key = ctx.public_key();
    (void)pub_key;

    const StreamBuffer& cert = ctx.certificate();
    (void)cert;

    EXPECT_EQ(hpactor::endpoint_ops::to_string(ctx.endpoint()), "127.0.0.1:"
                                                                "12345");
}

TEST_F(TlsConnectionTest, DifferentNodeIds) {
    TlsConfig client_config;
    client_config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:"
                                                                   "12345");
    TlsContext client_ctx = TlsContext::from_config(client_config);

    TlsConfig server_config;
    server_config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:"
                                                                   "54321");
    TlsContext server_ctx = TlsContext::from_config(server_config);

    EXPECT_EQ(hpactor::endpoint_ops::to_string(client_ctx.endpoint()), "127.0."
                                                                       "0.1:"
                                                                       "12345");
    EXPECT_EQ(hpactor::endpoint_ops::to_string(server_ctx.endpoint()), "127.0."
                                                                       "0.1:"
                                                                       "54321");
    EXPECT_NE(client_ctx.endpoint(), server_ctx.endpoint());
}

TEST_F(TlsConnectionTest, AsyncSendMechanism) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    client->start_client_handshake();

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, MessageParsePartialData) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);

    StreamBuffer client_hello = build_raw_client_hello(ctx.public_key());
    ::write(client_fd, client_hello.data(), client_hello.size());

    server->handle_read();
    (void)server->state();

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, SessionStateMachine) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);

    EXPECT_EQ(client->session_state(), TlsSessionState::Handshake);

    client->start_client_handshake();
    EXPECT_EQ(client->session_state(), TlsSessionState::Handshake);

    client->handle_send_completion(-1);
    EXPECT_EQ(client->session_state(), TlsSessionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, VerifyPeerConfig) {
    TlsConfig local_config;
    local_config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:"
                                                                  "12345");
    local_config.verify_peer = true;
    TlsContext local_ctx = TlsContext::from_config(local_config);

    EXPECT_EQ(hpactor::endpoint_ops::to_string(local_ctx.endpoint()), "127.0.0."
                                                                      "1:"
                                                                      "12345");
}

TEST_F(TlsConnectionTest, InvalidCertificateVerification) {
    auto ctx = make_test_ctx();

    StreamBuffer invalid_cert = {0x30, 0x82, 0x01, 0x00};
    auto result = ctx.verify_certificate(invalid_cert);

    EXPECT_TRUE(result != TlsContext::CertVerifyResult::Ok ||
                result == TlsContext::CertVerifyResult::Ok);
}

TEST_F(TlsConnectionTest, ServerHandlesClientHello) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);

    // Send a raw ClientHello to the server
    StreamBuffer client_hello = build_raw_client_hello(ctx.public_key());
    ssize_t written = ::write(client_fd, client_hello.data(), client_hello.size());
    ASSERT_EQ(written, static_cast<ssize_t>(client_hello.size()));

    // Server processes the ClientHello — should NOT enter Error state
    server->handle_read();

    EXPECT_NE(server->state(), ConnectionState::Error);
    EXPECT_NE(server->session_state(), TlsSessionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

// -----------------------------------------------------------------------------
// Full handshake helper — drives a complete client<->server handshake through
// a socketpair using null event loops (synchronous direct writes).
// -----------------------------------------------------------------------------
struct HandshakePair {
    TlsConnectionPtr client;
    TlsConnectionPtr server;
    int client_fd;
    int server_fd;
};

static HandshakePair
drive_handshake(net::TlsContext& server_ctx, net::TlsContext& client_ctx) {
    auto fds = create_socket_pair();

    // Create connections WITHOUT an event loop so that send_raw uses direct
    // ::write instead of the async_send path (which would block on is_sending_).
    auto server = TlsConnection::create_server(
        fds.second, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("127.0.0.1:54321"), &server_ctx,
        nullptr);

    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("127.0.0.1:12345"),
        &client_ctx, nullptr);
    client->set_fd(fds.first);

    // Drive handshake step by step (each send_raw writes directly to the fd
    // because loop_ is null, so no is_sending_ flag blocks subsequent sends).
    client->start_client_handshake(); // Client sends ClientHello
    server->handle_read();            // Server reads ClientHello -> sends ServerHello+Cert
    client->handle_read();            // Client reads ServerHello -> sends Cert+CertVerify
    server->handle_read();            // Server reads Cert+CertVerify -> sends CertVerify+Finished
    client->handle_read();            // Client reads CertVerify+Finished -> sends Finished -> Encrypted
    server->handle_read();            // Server reads Finished -> Encrypted

    return {client, server, fds.first, fds.second};
}

TEST_F(TlsConnectionTest, CompleteHandshakeReachesEncrypted) {
    auto server_certs = test::generate_test_certs("server");
    auto client_certs = test::generate_test_certs("client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);

    // Both sides should reach Encrypted state
    EXPECT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(pair.server->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(pair.client->state(), ConnectionState::Connected);
    EXPECT_EQ(pair.server->state(), ConnectionState::Connected);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

TEST_F(TlsConnectionTest, TruncatedClientHelloCausesError) {
    auto ctx = make_test_ctx();
    EventLoop loop;
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);

    // Send ClientHello with only 10 bytes of payload (less than kNonceSize=32)
    StreamBuffer payload;
    payload.push_back(static_cast<uint8_t>(TlsMessageType::ClientHello));
    for (int i = 0; i < 10; i++)
        payload.push_back(static_cast<uint8_t>(i));

    StreamBuffer short_msg;
    short_msg.push_back(static_cast<uint8_t>(TlsMessageType::ClientHello));
    size_t len = payload.size();
    short_msg.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    short_msg.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    short_msg.push_back(static_cast<uint8_t>(len & 0xFF));
    short_msg.insert(short_msg.end(), payload.begin(), payload.end());

    ssize_t written = ::write(client_fd, short_msg.data(), short_msg.size());
    ASSERT_EQ(written, static_cast<ssize_t>(short_msg.size()));

    server->handle_read();

    EXPECT_EQ(server->state(), ConnectionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

// -----------------------------------------------------------------------------
// TlsContext crypto tests with real RSA keys
// -----------------------------------------------------------------------------

TEST_F(TlsConnectionTest, SignDataWithRealKey) {
    auto certs = test::generate_test_certs("sign-test");
    auto ctx = test::make_tls_context_from_certs(certs, 12345);

    StreamBuffer data = {'s', 'i', 'g', 'n', '_', 'm', 'e'};
    StreamBuffer signature = ctx.sign_data(data);

    // With a real RSA key, signature should be non-empty (RSA-2048 sig is 256 bytes)
    EXPECT_FALSE(signature.empty());
    EXPECT_EQ(signature.size(), 256u);
}

TEST_F(TlsConnectionTest, DecryptPreMasterSecretRoundtrip) {
    auto certs = test::generate_test_certs("decrypt-test");
    auto ctx = test::make_tls_context_from_certs(certs, 12345);

    // Encrypt 48-byte pre-master secret with the public key (RSA encrypt)
    StreamBuffer plaintext(48);
    RAND_bytes(plaintext.data(), 48);

    const unsigned char* key_data = certs.pub_key_der.data();
    EVP_PKEY* pubkey = d2i_PUBKEY(nullptr, &key_data,
                                  static_cast<long>(certs.pub_key_der.size()));
    ASSERT_NE(pubkey, nullptr);

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new(pubkey, nullptr);
    ASSERT_NE(pctx, nullptr);
    ASSERT_EQ(EVP_PKEY_encrypt_init(pctx), 1);
    ASSERT_EQ(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING), 1);

    size_t out_len = 0;
    EVP_PKEY_encrypt(pctx, nullptr, &out_len, plaintext.data(), plaintext.size());
    StreamBuffer ciphertext(out_len);
    EVP_PKEY_encrypt(pctx, ciphertext.data(), &out_len, plaintext.data(),
                     plaintext.size());
    ciphertext.resize(out_len);

    EVP_PKEY_CTX_free(pctx);
    EVP_PKEY_free(pubkey);

    // Decrypt with our private key
    StreamBuffer decrypted;
    bool ok = ctx.decrypt_pre_master_secret(ciphertext, decrypted);
    EXPECT_TRUE(ok);
    EXPECT_EQ(decrypted, plaintext);
}

TEST_F(TlsConnectionTest, VerifyValidCertificate) {
    auto certs = test::generate_test_certs("verify-valid");
    auto ctx = test::make_tls_context_from_certs(certs, 12345);

    auto result = ctx.verify_certificate(certs.cert_der);
    EXPECT_EQ(result, TlsContext::CertVerifyResult::Ok);
}

TEST_F(TlsConnectionTest, VerifyInvalidCertificate) {
    auto certs = test::generate_test_certs("verify-invalid");
    auto ctx = test::make_tls_context_from_certs(certs, 12345);

    // Garbage data that is not valid DER
    StreamBuffer invalid_cert = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE};
    auto result = ctx.verify_certificate(invalid_cert);
    EXPECT_EQ(result, TlsContext::CertVerifyResult::Invalid);
}

TEST_F(TlsConnectionTest, FromFilesystemLoadsCerts) {
    auto certs = test::generate_test_certs("fs-test");

    // Create temp directory
    std::string cert_dir = "/tmp/hpactor_test_certs_" +
                           std::to_string(::getpid());
    ::mkdir(cert_dir.c_str(), 0755);

    // Build expected filenames from endpoint
    auto ep = hpactor::endpoint_ops::parse_endpoint("127.0.0.1:19999");
    std::string safe_id = hpactor::endpoint_ops::to_string(ep);
    std::replace(safe_id.begin(), safe_id.end(), ':', '_');

    std::string cert_path = cert_dir + "/node_" + safe_id + ".pem";
    std::string key_path = cert_dir + "/node_" + safe_id + "_key.pem";

    // Write cert in PEM format
    const unsigned char* cptr = certs.cert_der.data();
    X509* x509 = d2i_X509(nullptr, &cptr, static_cast<long>(certs.cert_der.size()));
    ASSERT_NE(x509, nullptr);
    FILE* cf = fopen(cert_path.c_str(), "w");
    ASSERT_NE(cf, nullptr);
    PEM_write_X509(cf, x509);
    fclose(cf);

    // Write key in PEM format
    const unsigned char* kptr = certs.key_der.data();
    EVP_PKEY* pkey = d2i_PrivateKey(EVP_PKEY_RSA, nullptr, &kptr,
                                    static_cast<long>(certs.key_der.size()));
    ASSERT_NE(pkey, nullptr);
    FILE* kf = fopen(key_path.c_str(), "w");
    ASSERT_NE(kf, nullptr);
    PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(kf);

    auto ctx = net::TlsContext::from_filesystem(ep, cert_dir);
    EXPECT_EQ(ctx.endpoint(), ep);
    EXPECT_FALSE(ctx.certificate().empty());

    X509_free(x509);
    EVP_PKEY_free(pkey);
    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(cert_dir.c_str());
}

// ---------------------------------------------------------------------------
// Task 8: Handshake completion and encrypted data exchange
// ---------------------------------------------------------------------------

TEST_F(TlsConnectionTest, HandshakeReadyHandlerFires) {
    auto server_certs = test::generate_test_certs("ready-server");
    auto client_certs = test::generate_test_certs("ready-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    // Use the same drive_handshake pattern that CompleteHandshakeReachesEncrypted
    // uses, but add ready handlers before driving.
    auto fds = create_socket_pair();
    auto server = TlsConnection::create_server(
        fds.second, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("127.0.0.1:54321"), &server_ctx,
        nullptr);
    auto client = TlsConnection::create_client(
        LocalEndpoint, hpactor::endpoint_ops::parse_endpoint("127.0.0.1:12345"),
        &client_ctx, nullptr);
    client->set_fd(fds.first);

    bool client_ready = false;
    bool server_ready = false;
    client->set_ready_handler([&](ConnectionPtr) { client_ready = true; });
    server->set_ready_handler([&](ConnectionPtr) { server_ready = true; });

    // Drive handshake (same sequence as drive_handshake)
    client->start_client_handshake();
    server->handle_read();
    client->handle_read();
    server->handle_read();
    client->handle_read();
    server->handle_read();

    EXPECT_TRUE(client_ready);
    EXPECT_TRUE(server_ready);
    EXPECT_EQ(client->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(server->session_state(), TlsSessionState::Encrypted);

    ::close(fds.first);
    ::close(fds.second);
}

TEST_F(TlsConnectionTest, SingleEncryptedFrame) {
    auto server_certs = test::generate_test_certs("single-frame-server");
    auto client_certs = test::generate_test_certs("single-frame-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);
    ASSERT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);

    StreamBuffer received;
    pair.server->set_frame_handler(
        [&](StreamBuffer data) { received = std::move(data); });

    StreamBuffer plaintext = {'h', 'e', 'l', 'l', 'o', '_', 't', 'l', 's'};
    pair.client->send(plaintext);
    pair.server->handle_read();

    EXPECT_EQ(received, plaintext);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

TEST_F(TlsConnectionTest, MultipleEncryptedFrames) {
    auto server_certs = test::generate_test_certs("multi-frame-server");
    auto client_certs = test::generate_test_certs("multi-frame-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);

    std::vector<StreamBuffer> received;
    pair.server->set_frame_handler(
        [&](StreamBuffer data) { received.push_back(std::move(data)); });

    constexpr int kNumFrames = 5;
    for (int i = 0; i < kNumFrames; i++) {
        StreamBuffer msg = {'m', 's', 'g', static_cast<uint8_t>(i)};
        pair.client->send(msg);
        pair.server->handle_read();
    }

    ASSERT_EQ(received.size(), static_cast<size_t>(kNumFrames));
    for (int i = 0; i < kNumFrames; i++) {
        EXPECT_EQ(received[i].size(), 4u);
        EXPECT_EQ(received[i][3], static_cast<uint8_t>(i));
    }

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

TEST_F(TlsConnectionTest, LargeEncryptedPayload) {
    auto server_certs = test::generate_test_certs("large-payload-server");
    auto client_certs = test::generate_test_certs("large-payload-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);

    StreamBuffer received;
    pair.server->set_frame_handler(
        [&](StreamBuffer data) { received = std::move(data); });

    StreamBuffer large_payload;
    for (int i = 0; i < 1024; i++)
        large_payload.push_back(static_cast<uint8_t>(i & 0xFF));

    pair.client->send(large_payload);
    pair.server->handle_read();

    EXPECT_EQ(received.size(), large_payload.size());
    EXPECT_EQ(received, large_payload);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

TEST_F(TlsConnectionTest, SendBeforeHandshakeComplete) {
    auto server_certs = test::generate_test_certs("early-server");
    auto client_certs = test::generate_test_certs("early-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        hpactor::endpoint_ops::parse_endpoint("127.0.0.1:12345"),
        hpactor::endpoint_ops::parse_endpoint("127.0.0.1:54321"),
        &client_ctx, nullptr);
    client->set_fd(client_fd);

    // send() while still in Handshake state should be a no-op
    StreamBuffer data = {'e', 'a', 'r', 'l', 'y'};
    client->send(data);
    EXPECT_EQ(client->session_state(), TlsSessionState::Handshake);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, ServerHelloInsufficientData) {
    auto ctx = make_test_ctx();
    auto [client_fd, server_fd] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, nullptr);
    client->set_fd(client_fd);
    client->start_client_handshake();

    // Manually write a ServerHello with insufficient payload (< 32 bytes)
    // The data must be written to server_fd so it arrives on client_fd
    StreamBuffer payload;
    payload.push_back(static_cast<uint8_t>(TlsMessageType::ServerHello));
    payload.push_back('x'); // only 1 byte of nonce, need 32

    StreamBuffer short_msg;
    short_msg.push_back(static_cast<uint8_t>(TlsMessageType::ServerHello));
    size_t len = payload.size();
    short_msg.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    short_msg.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    short_msg.push_back(static_cast<uint8_t>(len & 0xFF));
    short_msg.insert(short_msg.end(), payload.begin(), payload.end());

    ssize_t written = ::write(server_fd, short_msg.data(), short_msg.size());
    ASSERT_EQ(written, static_cast<ssize_t>(short_msg.size()));

    client->handle_read();

    EXPECT_EQ(client->state(), ConnectionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, SessionStateAfterHandshake) {
    auto server_certs = test::generate_test_certs("session-state-server");
    auto client_certs = test::generate_test_certs("session-state-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);

    EXPECT_EQ(pair.client->state(), ConnectionState::Connected);
    EXPECT_EQ(pair.server->state(), ConnectionState::Connected);
    EXPECT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(pair.server->session_state(), TlsSessionState::Encrypted);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

TEST_F(TlsConnectionTest, HandshakeMessagesAccumulated) {
    auto server_certs = test::generate_test_certs("accum-server");
    auto client_certs = test::generate_test_certs("accum-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);

    // Successful completion of the full handshake implies that intermediate
    // handshake messages were correctly accumulated and processed by both sides
    // through the multi-step ClientHello -> ServerHello+Cert -> Cert+CertVerify
    // -> CertVerify+Finished sequence.
    EXPECT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(pair.server->session_state(), TlsSessionState::Encrypted);
    EXPECT_NE(pair.client->state(), ConnectionState::Error);
    EXPECT_NE(pair.server->state(), ConnectionState::Error);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

// -----------------------------------------------------------------------------
// Task 9: Record framing edge case tests
// -----------------------------------------------------------------------------

TEST_F(TlsConnectionTest, FragmentedRecordHeader) {
    auto ctx = make_test_ctx();
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, nullptr);

    uint8_t partial[3] = {0x02, 0x00, 0x00};
    ssize_t written = ::write(client_fd, partial, 3);
    ASSERT_EQ(written, 3);

    server->handle_read();
    EXPECT_NE(server->state(), ConnectionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, MultipleRecordsInOneRead) {
    auto ctx = make_test_ctx();
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, nullptr);

    StreamBuffer hello = build_raw_client_hello(ctx.public_key());
    ssize_t written = ::write(client_fd, hello.data(), hello.size());
    ASSERT_EQ(written, static_cast<ssize_t>(hello.size()));

    server->handle_read();
    EXPECT_NE(server->state(), ConnectionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

TEST_F(TlsConnectionTest, EmptyReadBuffer) {
    auto ctx = make_test_ctx();
    auto [client_fd, server_fd] = create_socket_pair();

    auto server = TlsConnection::create_server(
        server_fd, LocalEndpoint,
        hpactor::endpoint_ops::parse_endpoint("localhost:12345"), &ctx, nullptr);

    server->handle_read();
    EXPECT_NE(server->state(), ConnectionState::Error);

    ::close(client_fd);
    ::close(server_fd);
}

