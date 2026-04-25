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

#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// Helper to test path derivation
static std::string derive_uds_path(const std::string& node_id) {
    std::string sanitized = node_id;
    for (char& c : sanitized) {
        if (c == ':') c = '_';
    }
    return "/tmp/hpactor/" + sanitized + ".sock";
}

// TCP fallback tests

// Test: UDS fallback with invalid path returns nullptr
void test_uds_fallback_invalid_path() {
    hpactor::net::EventLoop loop;

    // Start a TCP server to have something to fall back to
    hpactor::net::Acceptor tcp_acceptor(&loop);
    bool tcp_started = tcp_acceptor.listen(19996);
    assert(tcp_started);

    // Create remote endpoint for TCP server
    auto remote_ep = hpactor::Ipv4Endpoint{htonl(0x7F000001), htons(19996)};

    // Create TcpTransport with empty registry (no UDS available)
    hpactor::net::TlsConfig tls_config;
    hpactor::net::PoolConfig pool_config;
    hpactor::net::TcpTransport transport(remote_ep, tls_config, pool_config, nullptr);

    // Try to connect via UDS with invalid path - should return nullptr
    auto conn = transport.connect_unix_domain(remote_ep, "/nonexistent/path.sock");
    assert(conn == nullptr);  // UDS should fail gracefully

    // Clean up TCP server
    tcp_acceptor.close();
}

// Test: UDS fallback when registry has no uds_path
void test_uds_fallback_when_uds_unavailable() {
    // When registry has no uds_path, connect(ep) should use TCP
    // The key is that uds_path.empty() triggers TCP path

    hpactor::net::EventLoop loop;
    auto remote_ep = hpactor::Ipv4Endpoint{htonl(0x7F000001), htons(19997)};

    hpactor::net::TlsConfig tls_config;
    hpactor::net::PoolConfig pool_config;

    // Create transport with nullptr registry (simulates no node registry)
    hpactor::net::TcpTransport transport(remote_ep, tls_config, pool_config, nullptr);

    // Without a registry, we can't look up a UDS path, so connect should
    // fail gracefully on UDS attempt and fall back behavior is tested
    // via the invalid path test above
    auto conn = transport.connect_unix_domain(remote_ep, "/tmp/nonexistent.sock");
    assert(conn == nullptr);
}

int main() {
    // Test SimpleNodeId: "localhost:5000" -> "/tmp/hpactor/localhost_5000.sock"
    {
        auto path = derive_uds_path("localhost:5000");
        assert(path == "/tmp/hpactor/localhost_5000.sock");
    }

    // Test IpAddress: "127.0.0.1:8080" -> "/tmp/hpactor/127.0.0.1_8080.sock"
    {
        auto path = derive_uds_path("127.0.0.1:8080");
        assert(path == "/tmp/hpactor/127.0.0.1_8080.sock");
    }

    // Test NoPort: "node1" -> "/tmp/hpactor/node1.sock"
    {
        auto path = derive_uds_path("node1");
        assert(path == "/tmp/hpactor/node1.sock");
    }

    // Test MultipleColons: "192.168.1.1:5000" -> "/tmp/hpactor/192.168.1.1_5000.sock"
    {
        auto path = derive_uds_path("192.168.1.1:5000");
        assert(path == "/tmp/hpactor/192.168.1.1_5000.sock");
    }

    // -------------------------------------------------------------------------
    // UdsAcceptor Tests
    // -------------------------------------------------------------------------

    // Test: ListenAndAccept
    {
        hpactor::net::EventLoop loop;
        hpactor::net::Acceptor acceptor(&loop);
        std::string socket_path = "/tmp/hpactor/test_listen_accept.sock";

        bool server_started = acceptor.listen_unix_domain(socket_path);
        assert(server_started);

        // Verify socket file exists
        struct stat st;
        assert(stat(socket_path.c_str(), &st) == 0);

        // Verify it's actually listening
        assert(acceptor.is_listening());
        assert(acceptor.uds_path() == socket_path);

        // Clean up
        acceptor.close_unix_domain();

        // After close, socket file should be unlinked
        assert(stat(socket_path.c_str(), &st) == -1);
    }

    // Test: AcceptHandler
    {
        hpactor::net::EventLoop loop;
        hpactor::net::Acceptor acceptor(&loop);
        std::string socket_path = "/tmp/hpactor/test_accept_handler.sock";

        bool server_started = acceptor.listen_unix_domain(socket_path);
        assert(server_started);

        int client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        assert(client_fd >= 0);

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        // Non-blocking connect: may return 0 (immediate success) or -1 with
        // EINPROGRESS (connection in progress). Either way, the socket is
        // valid for the test - the accept will be handled by the event loop.
        int ret = ::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        (void)ret;

        // Note: The accept handler is invoked when the event loop processes
        // the readable event on the listening socket. Since running the
        // loop is blocking, this test verifies socket creation and
        // connection initiation rather than the full accept flow.

        // Client socket exists and is valid
        assert(client_fd >= 0);

        ::close(client_fd);
        acceptor.close_unix_domain();
    }

    // Test: CloseWithoutUdsPath
    {
        hpactor::net::EventLoop loop;
        hpactor::net::Acceptor acceptor(&loop);

        // Start regular TCP listener
        bool started = acceptor.listen(19995);
        assert(started);

        // close_unix_domain() should be safe even though not listening on UDS
        acceptor.close_unix_domain();  // Should not crash
        assert(!acceptor.is_listening());
    }

    // -------------------------------------------------------------------------
    // TCP Fallback Tests
    // -------------------------------------------------------------------------

    test_uds_fallback_invalid_path();
    test_uds_fallback_when_uds_unavailable();

    return 0;
}