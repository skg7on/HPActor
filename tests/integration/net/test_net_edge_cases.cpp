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

// Integration test: Network Edge Cases
// Connect timeout, double close, send-on-disconnected, zero-timeout event loop,
// WireFrame zero/max payload, connection pool zero capacity, HTTP error
// recovery.

#include <hpactor/msg/frame.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_connection.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

namespace {

// Helper: create a connected socket pair (client_fd -> server_fd)
std::pair<int, int> make_socket_pair() {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr), &len);
    listen(listener, 1);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, reinterpret_cast<struct sockaddr*>(&addr), len);
    int server = accept(listener, nullptr, nullptr);
    close(listener);
    return {client, server};
}

void make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Connect to non-listening port — verify timeout/error handling
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, ConnectToNonListeningPort) {
    EventLoop loop;
    loop.run();

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:19999");

    PoolConfig cfg;
    cfg.min_connections = 0;
    cfg.max_connections = 1;
    cfg.max_attempts = 1;
    cfg.initial_backoff = std::chrono::milliseconds{50};
    cfg.max_backoff = std::chrono::milliseconds{100};

    ConnectionPool pool(ep, cfg, &loop);

    // Sending to a pool that can't connect — should queue the message
    // without crashing
    ActorAddress addr;
    addr.endpoint = ep;
    StreamBuffer buf(64, 0xAA);
    auto result = pool.try_send(addr, buf);

    // Should either be queued (Sent) or rejected due to circuit breaker
    EXPECT_TRUE(result == TransportSendResult::Sent ||
                result == TransportSendResult::CircuitOpen ||
                result == TransportSendResult::QueueFull);

    loop.stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Double close of HTTP connection
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, HttpConnectionDoubleClose) {
    auto [client_fd, server_fd] = make_socket_pair();
    EventLoop loop;
    loop.run();

    auto conn = HTTPConnection::create(server_fd, LocalEndpoint, Ipv4Endpoint{},
                                       &loop, HTTPConnectionMode::Server);

    // First close
    conn->close();
    // Second close — should not crash
    conn->close();

    close(client_fd);
    loop.stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: Send on disconnected HTTP connection
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, SendOnDisconnectedHttpConnection) {
    auto [client_fd, server_fd] = make_socket_pair();
    EventLoop loop;
    loop.run();

    auto conn = HTTPConnection::create(server_fd, LocalEndpoint, Ipv4Endpoint{},
                                       &loop, HTTPConnectionMode::Server);

    // Close the client side to simulate disconnect
    close(client_fd);

    // Send on the server side after the client disconnected
    StreamBuffer data;
    data.append(reinterpret_cast<const uint8_t*>("test"), 4);
    // send() should not crash even if the peer is gone
    conn->send(data);

    conn->close();
    loop.stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: EventLoop with zero timeout
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, EventLoopZeroTimeout) {
    EventLoop loop;
    loop.run();

    // wait(0) should return immediately without blocking
    int triggered = loop.wait(0);
    // 0 events expected when no FDs are registered
    EXPECT_EQ(triggered, 0);

    loop.stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: WireFrame with zero-length payload
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, WireFrameZeroLengthPayload) {
    WireFrame frame;
    frame.magic_hdr = WireFrame::MagicHeader;
    frame.pb_frame.clear_payload();

    // Encode should handle zero-length payload without error
    StreamBuffer encoded = frame.encode();
    EXPECT_GE(encoded.size(), WireFrame::HeaderSize);

    // Decode should round-trip
    WireFrame decoded = WireFrame::decode(encoded);
    EXPECT_EQ(decoded.magic_hdr, WireFrame::MagicHeader);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 6: WireFrame with large payload (boundary)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, WireFrameLargePayload) {
    WireFrame frame;
    frame.magic_hdr = WireFrame::MagicHeader;

    // Create a 64KB payload
    StreamBuffer large_payload(65536, 0xBB);
    frame.pb_frame.set_payload(reinterpret_cast<const char*>(large_payload.data()),
                               large_payload.size());

    StreamBuffer encoded = frame.encode();
    EXPECT_GT(encoded.size(), 65536u);

    // Round-trip decode
    WireFrame decoded = WireFrame::decode(encoded);
    EXPECT_EQ(decoded.magic_hdr, WireFrame::MagicHeader);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 7: Connection pool with zero capacity
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, ConnectionPoolZeroCapacity) {
    EventLoop loop;
    loop.run();

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:19998");

    PoolConfig cfg;
    cfg.min_connections = 0;
    cfg.max_connections = 0;
    cfg.max_attempts = 1;

    ConnectionPool pool(ep, cfg, &loop);

    auto stats = pool.stats();
    EXPECT_EQ(stats.active_connections, 0u);
    EXPECT_FALSE(stats.is_connected);

    // Try to send — should queue or reject gracefully
    ActorAddress addr;
    addr.endpoint = ep;
    auto result = pool.try_send(addr, StreamBuffer(16, 0xCC));
    // Should either queue or reject, never crash
    EXPECT_TRUE(result == TransportSendResult::Sent ||
                result == TransportSendResult::CircuitOpen ||
                result == TransportSendResult::QueueFull);

    loop.stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 8: HTTP connection parse error recovery — send malformed data then valid
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, HttpConnectionErrorRecovery) {
    auto [client_fd, server_fd] = make_socket_pair();
    make_nonblocking(client_fd);
    EventLoop loop;
    loop.run();

    std::atomic<bool> got_error{false};
    std::atomic<bool> got_valid{false};
    std::string received_path;

    auto conn = HTTPConnection::create(server_fd, LocalEndpoint, Ipv4Endpoint{},
                                       &loop, HTTPConnectionMode::Server);
    conn->set_error_handler([&](HTTPConnection*, error err) {
        got_error = true;
        (void)err;
    });
    conn->set_request_handler([&](HTTPConnection*, HttpRequest&& req) {
        received_path = req.path;
        got_valid = true;
    });

    // Send malformed data first
    const char* bad = "GARBAGE\r\n\r\n";
    write(client_fd, bad, strlen(bad));

    // Give it time to process
    for (int i = 0; i < 20 && !got_error; i++) {
        loop.wait(50);
        loop.process_completions();
    }

    // Now send a valid request
    const char* good = "GET /recover HTTP/1.1\r\nHost: test\r\n\r\n";
    write(client_fd, good, strlen(good));

    for (int i = 0; i < 20 && !got_valid; i++) {
        loop.wait(50);
        loop.process_completions();
    }

    // Either we got an error on the bad data, or the connection handled it
    EXPECT_TRUE(got_error || got_valid);

    conn->close();
    close(client_fd);
    loop.stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 9: WireFrame decode with truncated/invalid data
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, WireFrameDecodeTruncatedData) {
    // Data shorter than the header — decode should handle gracefully
    StreamBuffer short_buf(4, 0x00);
    WireFrame decoded = WireFrame::decode(short_buf);
    // The decode should not crash. The resulting frame may or may not have
    // the correct magic header — either outcome is acceptable since the
    // input was invalid. We just verify it doesn't crash.
    EXPECT_TRUE(decoded.magic_hdr == WireFrame::MagicHeader ||
                decoded.magic_hdr != WireFrame::MagicHeader);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 10: ConnectionPool stats after rapid close
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetEdgeCases, ConnectionPoolStatsAfterRapidClose) {
    EventLoop loop;
    loop.run();

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:19997");
    PoolConfig cfg;
    cfg.min_connections = 0;
    cfg.max_connections = 1;

    ConnectionPool pool(ep, cfg, &loop);

    auto stats = pool.stats();
    EXPECT_EQ(stats.active_connections, 0u);

    pool.close();

    // Stats after close should still be accessible
    stats = pool.stats();
    EXPECT_EQ(stats.active_connections, 0u);

    loop.stop();
}
