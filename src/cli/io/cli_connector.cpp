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

#include <hpactor/cli/io/cli_connector.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/tls_context.hpp>
#include <hpactor/net/transport.hpp>

#include <arpa/inet.h>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliConnector::CliConnector() = default;

CliConnector::~CliConnector() {
    disconnect();
}

// ---------------------------------------------------------------------------
// connect_tcp
// ---------------------------------------------------------------------------

int CliConnector::connect_tcp(const std::string& host, uint16_t port,
                              std::chrono::milliseconds /*timeout*/) {
    disconnect();

    // Resolve host to IP for the EndPoint key
    struct in_addr ip_addr{};
    if (::inet_pton(AF_INET, host.c_str(), &ip_addr) != 1)
        return -1;

    Ipv4Endpoint local_ep{0, 0}; // any address, any port
    Ipv4Endpoint remote_ep{ip_addr.s_addr, 0};

    net::TlsConfig tls_cfg{};
    net::PoolConfig pool_cfg{};
    pool_cfg.min_connections = 1;
    pool_cfg.max_connections = 1;

    transport_ = std::make_unique<net::TcpTransport>(local_ep, tls_cfg, pool_cfg);
    conn_ = transport_->connect(remote_ep, host, port);
    if (!conn_)
        return -1;

    fd_ = conn_->fd();
    return fd_;
}

// ---------------------------------------------------------------------------
// connect_uds
// ---------------------------------------------------------------------------

int CliConnector::connect_uds(const std::string& path,
                              std::chrono::milliseconds /*timeout*/) {
    disconnect();

    Ipv4Endpoint local_ep{0, 0}; // any address, any port
    // Use a synthetic endpoint for the pool key
    Ipv4Endpoint remote_ep{0x7F000001, 0};

    net::TlsConfig tls_cfg{};
    net::PoolConfig pool_cfg{};
    pool_cfg.min_connections = 1;
    pool_cfg.max_connections = 1;

    transport_ = std::make_unique<net::TcpTransport>(local_ep, tls_cfg, pool_cfg);
    conn_ = transport_->connect_unix_domain(remote_ep, path);
    if (!conn_)
        return -1;

    fd_ = conn_->fd();
    return fd_;
}

// ---------------------------------------------------------------------------
// disconnect
// ---------------------------------------------------------------------------

void CliConnector::disconnect() {
    fd_ = -1;
    conn_.reset();
    transport_.reset();
}

bool CliConnector::is_connected() const {
    return conn_ && conn_->state() == net::ConnectionState::Connected;
}

} // namespace cli
} // namespace hpactor
