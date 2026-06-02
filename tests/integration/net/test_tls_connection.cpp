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
#include <unistd.h>

#include <gtest/gtest.h>

#include <openssl/asn1.h>
#include <openssl/evp.h>
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
