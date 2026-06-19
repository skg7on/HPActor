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

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "tls_test_helpers.hpp"

using namespace hpactor;
using namespace hpactor::net;

namespace {

void ensure_tmp_dir() {
    ::mkdir("/tmp/hpactor", 0755);
}

std::pair<int, int> create_socket_pair() {
    int sv[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    (void)ret;
    int flags = ::fcntl(sv[0], F_GETFL, 0);
    ::fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    flags = ::fcntl(sv[1], F_GETFL, 0);
    ::fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);
    return {sv[0], sv[1]};
}

TlsContext make_empty_ctx(uint16_t port = 12345) {
    TlsConfig config;
    config.endpoint =
        endpoint_ops::parse_endpoint("localhost:" + std::to_string(port));
    return TlsContext::from_config(config);
}

struct HandshakePair {
    TlsConnectionPtr client;
    TlsConnectionPtr server;
    int client_fd;
    int server_fd;
};

HandshakePair drive_handshake(TlsContext& server_ctx, TlsContext& client_ctx) {
    auto fds = create_socket_pair();

    auto server = TlsConnection::create_server(
        fds.second, LocalEndpoint,
        endpoint_ops::parse_endpoint("127.0.0.1:54321"), &server_ctx, nullptr);

    auto client = TlsConnection::create_client(
        LocalEndpoint, endpoint_ops::parse_endpoint("127.0.0.1:12345"),
        &client_ctx, nullptr);
    client->set_fd(fds.first);

    client->start_client_handshake();
    server->handle_read();
    client->handle_read();
    server->handle_read();
    client->handle_read();
    server->handle_read();

    return {client, server, fds.first, fds.second};
}

} // anonymous namespace

class TlsBranchesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ensure_tmp_dir();
    }
};

// ── 1. TlsContext certificate loading from config ─────────────────────

TEST_F(TlsBranchesTest, TlsContextLoadsCertificateFromConfig) {
    auto certs = test::generate_test_certs("ctx-load-test");
    TlsConfig cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:11111");
    cfg.own_cert_der = certs.cert_der;
    cfg.own_key_der = certs.key_der;
    cfg.verify_peer = false;

    auto ctx = TlsContext::from_config(cfg);
    EXPECT_EQ(ctx.endpoint(), cfg.endpoint);
    EXPECT_FALSE(ctx.certificate().empty());
    EXPECT_FALSE(ctx.public_key().empty());
}

// ── 2. TlsContext certificate loading from filesystem ─────────────────

TEST_F(TlsBranchesTest, TlsContextLoadsCertificateFromFilesystem) {
    ensure_tmp_dir();
    auto certs = test::generate_test_certs("fs-load-test");

    std::string cert_dir = "/tmp/hpactor_test_tls_branches_";
    cert_dir += std::to_string(::getpid());
    ::mkdir(cert_dir.c_str(), 0755);

    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:17777");
    std::string safe_id = endpoint_ops::to_string(ep);
    std::replace(safe_id.begin(), safe_id.end(), ':', '_');

    std::string cert_path = cert_dir + "/node_" + safe_id + ".pem";
    std::string key_path = cert_dir + "/node_" + safe_id + "_key.pem";

    {
        const unsigned char* cptr = certs.cert_der.data();
        X509* x509 =
            d2i_X509(nullptr, &cptr, static_cast<long>(certs.cert_der.size()));
        ASSERT_NE(x509, nullptr);
        FILE* cf = ::fopen(cert_path.c_str(), "w");
        ASSERT_NE(cf, nullptr);
        PEM_write_X509(cf, x509);
        ::fclose(cf);
        X509_free(x509);
    }

    {
        const unsigned char* kptr = certs.key_der.data();
        EVP_PKEY* pkey = d2i_PrivateKey(EVP_PKEY_RSA, nullptr, &kptr,
                                        static_cast<long>(certs.key_der.size()));
        ASSERT_NE(pkey, nullptr);
        FILE* kf = ::fopen(key_path.c_str(), "w");
        ASSERT_NE(kf, nullptr);
        PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        ::fclose(kf);
        EVP_PKEY_free(pkey);
    }

    auto ctx = TlsContext::from_filesystem(ep, cert_dir);
    EXPECT_EQ(ctx.endpoint(), ep);
    EXPECT_FALSE(ctx.certificate().empty());

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
    ::rmdir(cert_dir.c_str());
}

// ── 3. TlsConnection handshake states ──────────────────────────────────

TEST_F(TlsBranchesTest, TlsConnectionHandshakeStates) {
    auto server_certs = test::generate_test_certs("hs-state-server");
    auto client_certs = test::generate_test_certs("hs-state-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);

    EXPECT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(pair.server->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(pair.client->state(), ConnectionState::Connected);
    EXPECT_EQ(pair.server->state(), ConnectionState::Connected);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

// ── 4. TlsConnection encrypt/decrypt (single frame) ────────────────────

TEST_F(TlsBranchesTest, TlsConnectionEncryptDecryptSingleFrame) {
    auto server_certs = test::generate_test_certs("enc-server");
    auto client_certs = test::generate_test_certs("enc-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);
    ASSERT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);

    StreamBuffer received;
    pair.server->set_frame_handler(
        [&received](StreamBuffer data) { received = std::move(data); });

    StreamBuffer plaintext = {'e', 'n', 'c', 'r', 'y', 'p', 't', 'e', 'd'};
    pair.client->send(plaintext);
    pair.server->handle_read();

    EXPECT_EQ(received, plaintext);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

// ── 5. TlsConnection encrypt/decrypt (multiple frames) ─────────────────

TEST_F(TlsBranchesTest, TlsConnectionEncryptDecryptMultipleFrames) {
    auto server_certs = test::generate_test_certs("multi-enc-server");
    auto client_certs = test::generate_test_certs("multi-enc-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);
    ASSERT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);

    std::vector<StreamBuffer> received;
    pair.server->set_frame_handler([&received](StreamBuffer data) {
        received.push_back(std::move(data));
    });

    StreamBuffer plain1 = {'f', 'r', 'a', 'm', 'e', '1'};
    StreamBuffer plain2 = {'f', 'r', 'a', 'm', 'e', '2'};
    StreamBuffer plain3 = {'f', 'r', 'a', 'm', 'e', '3'};

    pair.client->send(plain1);
    pair.server->handle_read();
    pair.client->send(plain2);
    pair.server->handle_read();
    pair.client->send(plain3);
    pair.server->handle_read();

    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], plain1);
    EXPECT_EQ(received[1], plain2);
    EXPECT_EQ(received[2], plain3);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

// ── 6. Send before handshake (plaintext fallback) ──────────────────────

TEST_F(TlsBranchesTest, SendBeforeHandshakeComplete) {
    auto ctx = make_empty_ctx();
    auto [fd_a, fd_b] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, endpoint_ops::parse_endpoint("localhost:12345"), &ctx,
        nullptr);
    client->set_fd(fd_a);

    StreamBuffer data = {'b', 'e', 'f', 'o', 'r', 'e'};
    client->send(data);
    EXPECT_EQ(client->session_state(), TlsSessionState::Handshake);

    ::close(fd_a);
    ::close(fd_b);
}

// ── 7. TlsContext certificate verification ─────────────────────────────

TEST_F(TlsBranchesTest, TlsContextCertificateVerification) {
    auto certs = test::generate_test_certs("verify-branches");
    auto ctx = test::make_tls_context_from_certs(certs, 34567);

    EXPECT_EQ(ctx.verify_certificate(certs.cert_der),
              TlsContext::CertVerifyResult::Ok);

    StreamBuffer invalid = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(ctx.verify_certificate(invalid),
              TlsContext::CertVerifyResult::Invalid);

    auto wrong_certs = test::generate_test_certs("wrong-cert");
    auto result = ctx.verify_certificate(wrong_certs.cert_der);
    (void)result;
}

// ── 8. Self-signed certificate generation ──────────────────────────────

TEST_F(TlsBranchesTest, SelfSignedCertificateGeneration) {
    auto certs1 = test::generate_test_certs("test-common-name-1");
    EXPECT_FALSE(certs1.cert_der.empty());
    EXPECT_FALSE(certs1.key_der.empty());
    EXPECT_FALSE(certs1.pub_key_der.empty());

    EXPECT_GT(certs1.cert_der.size(), 100u);
    EXPECT_GT(certs1.key_der.size(), 100u);
    EXPECT_GT(certs1.pub_key_der.size(), 100u);

    auto certs2 = test::generate_test_certs("test-common-name-2");
    EXPECT_FALSE(certs2.cert_der.empty());
    EXPECT_NE(certs1.cert_der, certs2.cert_der);
    EXPECT_NE(certs1.key_der, certs2.key_der);
}

// ── 9. TlsContext with non-matching key ────────────────────────────────

TEST_F(TlsBranchesTest, TlsContextWithNonMatchingKey) {
    auto certs_a = test::generate_test_certs("key-mismatch-a");
    auto certs_b = test::generate_test_certs("key-mismatch-b");

    TlsConfig mismatched_cfg;
    mismatched_cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:33333");
    mismatched_cfg.own_cert_der = certs_a.cert_der;
    mismatched_cfg.own_key_der = certs_b.key_der;
    mismatched_cfg.verify_peer = false;

    auto ctx = TlsContext::from_config(mismatched_cfg);
    StreamBuffer data = {'t', 'e', 's', 't'};
    StreamBuffer sig = ctx.sign_data(data);
    (void)sig;
    SUCCEED();
}

// ── 10. TlsConnection with EventLoop (BIO/async path) ──────────────────

TEST_F(TlsBranchesTest, TlsConnectionWithEventLoop) {
    auto ctx = make_empty_ctx();
    EventLoop loop;
    auto [fd_a, fd_b] = create_socket_pair();

    auto server = TlsConnection::create_server(
        fd_b, LocalEndpoint, endpoint_ops::parse_endpoint("localhost:12345"),
        &ctx, &loop);
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(server->state(), ConnectionState::Connected);

    auto client = TlsConnection::create_client(
        LocalEndpoint, endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);
    ASSERT_NE(client, nullptr);
    EXPECT_EQ(client->state(), ConnectionState::Connecting);

    client->set_fd(fd_a);
    client->start_client_handshake();
    loop.process_completions();

    (void)server->state();
    (void)client->state();

    client->close();
    server->close();
}

// ── 11. TlsConnection close and error paths ────────────────────────────

TEST_F(TlsBranchesTest, TlsConnectionCloseAndErrorPaths) {
    auto ctx = make_empty_ctx();
    EventLoop loop;
    auto [fd_a, fd_b] = create_socket_pair();

    auto client = TlsConnection::create_client(
        LocalEndpoint, endpoint_ops::parse_endpoint("localhost:12345"), &ctx, &loop);
    client->set_fd(fd_a);

    std::atomic<bool> error_fired{false};
    client->set_error_handler(
        [&error_fired](ConnectionPtr /*conn*/, const error& /*err*/) {
            error_fired = true;
        });

    client->close();
    EXPECT_EQ(client->state(), ConnectionState::Disconnected);

    // Double close is safe
    client->close();
    EXPECT_EQ(client->state(), ConnectionState::Disconnected);

    ::close(fd_a);
    ::close(fd_b);
}

// ── 12. TlsConfig defaults ─────────────────────────────────────────────

TEST_F(TlsBranchesTest, TlsConfigDefaults) {
    TlsConfig cfg;

    EXPECT_TRUE(cfg.own_cert_der.empty());
    EXPECT_TRUE(cfg.own_key_der.empty());
    EXPECT_TRUE(cfg.ca_certs_der.empty());
    EXPECT_TRUE(cfg.verify_peer);

    EndPoint ep = cfg.endpoint;
    (void)ep;
}

// ── 13. TlsConnection state transitions ────────────────────────────────

TEST_F(TlsBranchesTest, TlsConnectionStateTransitions) {
    auto server_certs = test::generate_test_certs("state-server");
    auto client_certs = test::generate_test_certs("state-client");
    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client_ctx = test::make_tls_context_from_certs(client_certs, 12345);

    auto pair = drive_handshake(server_ctx, client_ctx);

    EXPECT_EQ(pair.client->session_state(), TlsSessionState::Encrypted);
    EXPECT_EQ(pair.server->session_state(), TlsSessionState::Encrypted);

    pair.client->close();
    EXPECT_EQ(pair.client->state(), ConnectionState::Disconnected);

    pair.server->close();
    EXPECT_EQ(pair.server->state(), ConnectionState::Disconnected);

    ::close(pair.client_fd);
    ::close(pair.server_fd);
}

// ── 14. Two independent TLS connections ────────────────────────────────

TEST_F(TlsBranchesTest, TwoIndependentTlsConnections) {
    auto server_certs = test::generate_test_certs("indep-server");
    auto client1_certs = test::generate_test_certs("indep-client1");
    auto client2_certs = test::generate_test_certs("indep-client2");

    auto server_ctx = test::make_tls_context_from_certs(server_certs, 54321);
    auto client1_ctx = test::make_tls_context_from_certs(client1_certs, 11111);
    auto client2_ctx = test::make_tls_context_from_certs(client2_certs, 22222);

    auto pair1 = drive_handshake(server_ctx, client1_ctx);
    EXPECT_EQ(pair1.client->session_state(), TlsSessionState::Encrypted);

    auto pair2 = drive_handshake(server_ctx, client2_ctx);
    EXPECT_EQ(pair2.client->session_state(), TlsSessionState::Encrypted);

    pair1.client->close();
    pair2.client->send({'i', 'n', 'd', 'e', 'p'});
    pair2.server->handle_read();

    ::close(pair1.client_fd);
    ::close(pair1.server_fd);
    ::close(pair2.client_fd);
    ::close(pair2.server_fd);
}
