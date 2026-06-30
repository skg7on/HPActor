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

#include <hpactor/net/tcp_transport.hpp>

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/log/logger.hpp>

#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// TcpTransport implementation
// -----------------------------------------------------------------------------

TcpTransport::TcpTransport(EndPoint endpoint, const TlsConfig& tls_config,
                           const PoolConfig& pool_config, NodeRegistry* registry)
    : endpoint_(endpoint), acceptor_(&loop_),
      tls_context_(TlsContext::from_config(tls_config)),
      pool_config_(pool_config), registry_(registry) {
    // Ensure UDS directory exists
    std::string uds_dir = "/tmp/hpactor";
    ::mkdir(uds_dir.c_str(), 0755); // Ignore error if exists

    // Set up completion callback to route send completions to TlsConnection
    completion_callback_ = [this](OpCompletion c) {
        if (c.type == OpType::Send) {
            auto it = connections_.find(c.fd);
            if (it != connections_.end()) {
                it->second->handle_send_completion(c.result);
            }
        }
    };
    loop_.set_completion_callback(completion_callback_);
}

TcpTransport::~TcpTransport() {
    stop_listening();
    // Abort all connection pools
    for (auto& [ep, pool] : pools_) {
        pool->abort();
    }
}

std::shared_ptr<ConnectionPool>
TcpTransport::get_or_create_pool(EndPoint remote_endpoint) {
    auto it = pools_.find(remote_endpoint);
    if (it != pools_.end()) {
        return it->second;
    }
    auto pool =
        std::make_shared<ConnectionPool>(remote_endpoint, pool_config_, &loop_);
    pools_[remote_endpoint] = pool;
    // Set RPC handler if one has been registered
    if (rpc_handler_) {
        pool->set_rpc_handler(rpc_handler_);
    }
    // Set actor message handler if one has been registered
    if (actor_msg_handler_) {
        pool->set_actor_message_handler(actor_msg_handler_);
    }
    // Propagate metrics ring buffer
    if (metrics_ring_buffer_) {
        pool->set_metrics_ring_buffer(metrics_ring_buffer_);
    }
    // Propagate unified inbound sink
    if (inbound_sink_.active()) {
        pool->set_inbound_frame_sink(inbound_sink_);
    }
    return pool;
}

ConnectionPtr TcpTransport::connect(EndPoint remote_endpoint,
                                    const std::string& /*host*/, uint16_t port) {
    // Build sockaddr from remote_endpoint — supports both IPv4 and IPv6
    struct sockaddr_storage addr_storage{};
    socklen_t addr_len = 0;
    int family = 0;

    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&remote_endpoint)) {
        family = AF_INET;
        auto* sa = reinterpret_cast<struct sockaddr_in*>(&addr_storage);
        sa->sin_family = AF_INET;
        sa->sin_port = htons(port);
        sa->sin_addr.s_addr = ipv4->addr;
        addr_len = sizeof(struct sockaddr_in);
    } else if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&remote_endpoint)) {
        family = AF_INET6;
        auto* sa = reinterpret_cast<struct sockaddr_in6*>(&addr_storage);
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(port);
        std::memcpy(sa->sin6_addr.s6_addr, ipv6->addr.data(), 16);
        sa->sin6_flowinfo = 0;
        sa->sin6_scope_id = 0;
        addr_len = sizeof(struct sockaddr_in6);
    } else {
        return nullptr;
    }

    int fd = ::socket(family, SOCK_STREAM, 0);
    if (fd < 0) {
        return nullptr;
    }

    // Set TCP_NODELAY
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int result = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr_storage),
                           addr_len);
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    auto pool = get_or_create_pool(remote_endpoint);
    bool use_tls = pool_config_.use_tls;
    ConnectionPtr conn;

    // Create connection in Connecting state — event loop registration is
    // deferred until the non-blocking connect completes.
    if (use_tls) {
        auto tls_conn = TlsConnection::create_client(endpoint_, remote_endpoint,
                                                     &tls_context_, &loop_);
        tls_conn->set_fd(fd);
        tls_conn->set_ready_handler(
            [pool](ConnectionPtr c) { pool->on_connection_ready(c); });
        tls_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        tls_conn->set_frame_handler([pool](StreamBuffer data) {
            pool->on_frame_received(std::move(data));
        });
        conn = tls_conn;
    } else {
        auto plain_conn = WireFrameConnection::create_connecting_client(
            fd, endpoint_, remote_endpoint, &loop_);
        plain_conn->set_ready_handler(
            [pool](ConnectionPtr c) { pool->on_connection_ready(c); });
        plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        plain_conn->set_frame_handler([pool](StreamBuffer data) {
            pool->on_frame_received(std::move(data));
        });
        plain_conn->set_max_inbound_frame_bytes(pool_config_.max_inbound_frame_bytes);
        conn = plain_conn;
    }

    // Add to pool and track early so the write_handler can find the connection
    pool->add_connection(conn);
    register_connection(conn, fd);

    // Handle non-blocking connect completion
    if (result < 0 && errno == EINPROGRESS) {
        if (!complete_connect(fd, use_tls)) {
            unregister_connection(fd);
            return nullptr;
        }
    } else {
        // Connected immediately (e.g. localhost) — set up read handler directly
        if (use_tls) {
            TlsConnection::setup_after_connect(
                std::static_pointer_cast<TlsConnection>(conn));
            static_cast<TlsConnection*>(conn.get())->start_client_handshake();
        } else {
            WireFrameConnection::setup_after_connect(
                std::static_pointer_cast<WireFrameConnection>(conn));
        }
    }

    return conn;
}

ConnectionPtr TcpTransport::connect(EndPoint remote_endpoint) {
    if (registry_ == nullptr) {
        return nullptr; // No registry configured
    }

    NodeEndpoint* ep = registry_->get(remote_endpoint);
    if (ep == nullptr) {
        return nullptr; // Unknown node
    }

    // Check if UDS path is available for this endpoint
    if (!ep->identity.uds_path.empty()) {
        return connect_unix_domain(remote_endpoint, ep->identity.uds_path);
    }

    // Resolve hostname to IP if needed
    std::string ip = host_resolver_.resolve(ep->identity.host);

    // Connect to resolved IP:port
    return connect(remote_endpoint, ip, ep->tcp_port);
}

ConnectionPtr TcpTransport::connect_unix_domain(EndPoint remote_endpoint,
                                                const std::string& socket_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return nullptr;
    }

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int result =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    // Note: TLS over UDS is not supported per design decision.
    // UDS connections always use WireFrameConnection.
    auto pool = get_or_create_pool(remote_endpoint);
    auto plain_conn = WireFrameConnection::create_connecting_client(
        fd, endpoint_, remote_endpoint, &loop_);
    plain_conn->set_ready_handler(
        [pool](ConnectionPtr c) { pool->on_connection_ready(c); });
    plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
        pool->on_connection_error(c, e);
    });
    plain_conn->set_frame_handler([pool](StreamBuffer data) {
        pool->on_frame_received(std::move(data));
    });

    pool->add_connection(plain_conn);
    register_connection(plain_conn, fd);

    // Handle non-blocking connect completion
    if (result < 0 && errno == EINPROGRESS) {
        if (!complete_connect(fd, /*use_tls=*/false)) {
            unregister_connection(fd);
            return nullptr;
        }
    } else {
        WireFrameConnection::setup_after_connect(plain_conn);
    }

    return plain_conn;
}

bool TcpTransport::complete_connect(int fd, bool use_tls) {
    // Ensure event loop backend is started
    if (!loop_.is_running()) {
        loop_.run();
    }

    // Shared flag to coordinate between write_handler and timeout —
    // whichever fires first wins; the other becomes a no-op.
    auto done = std::make_shared<bool>(false);

    // Register for Write events — the fd becomes writable when the
    // non-blocking TCP handshake completes (success or error).
    loop_.add_fd(fd, EventLoop::Event::Write);

    loop_.set_write_handler(fd, [this, fd, use_tls, done](int event_fd) {
        (void)event_fd;
        if (*done)
            return;
        *done = true;

        loop_.clear_write_handler(fd);

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);

        auto it = connections_.find(fd);
        if (it == connections_.end())
            return;

        if (so_error != 0) {
            it->second->set_state(ConnectionState::Error);
            HPACTOR_LOG_ERROR(log::LogCategory::kNetwork, ActorId{0}, 0,
                              "connection error");
            return;
        }

        // Connect succeeded — complete the per-type setup
        if (use_tls) {
            TlsConnection::setup_after_connect(
                std::static_pointer_cast<TlsConnection>(it->second));
            static_cast<TlsConnection*>(it->second.get())->start_client_handshake();
        } else {
            WireFrameConnection::setup_after_connect(
                std::static_pointer_cast<WireFrameConnection>(it->second));
        }
    });

    // Timeout via event loop timer — fires after 5s if the connect
    // hasn't completed.
    loop_.run_after(
        [this, fd, done]() {
            if (*done)
                return;
            *done = true;

            loop_.clear_write_handler(fd);

            auto it = connections_.find(fd);
            if (it != connections_.end() &&
                it->second->state() == ConnectionState::Connecting) {
                it->second->set_state(ConnectionState::Error);
            }
        },
        5000);

    // Wait for the write handler or timeout to fire.  The fd becomes
    // writable when the TCP handshake completes (success or error).
    while (!*done) {
        loop_.process_completions();
        loop_.wait(100);
    }

    // Return true only if the connection reached Connected state.
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return false;
    return it->second->state() == ConnectionState::Connected;
}

void TcpTransport::listen(uint16_t port) {
    acceptor_.set_accept_handler(
        [this](int client_fd, EndPoint ep) { handle_accept(client_fd, ep); });
    acceptor_.listen(port);
}

void TcpTransport::stop_listening() {
    acceptor_.close();
}

TransportSendResult
TcpTransport::try_send(const ActorAddress& target, const StreamBuffer& encoded) {
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    FAULT_INJECT("hpactor.transport.send.drop") {
        return TransportSendResult::Sent; // claim success, silently drop
    }
    FAULT_INJECT("hpactor.transport.send.delay") {
        _fc->stall(hpactor::fault::FaultDomain::kTransport, /*delay_ticks=*/3);
    }
    FAULT_INJECT("hpactor.transport.send.corrupt") {
        if (encoded.size() > 0) {
            StreamBuffer corrupted(encoded);
            corrupted.data()[0] ^= 0xFF;
            auto pool = get_or_create_pool(target.endpoint);
            return pool->try_send(target, corrupted);
        }
    }
    auto pool = get_or_create_pool(target.endpoint);
    return pool->try_send(target, encoded);
}

TransportSendResult TcpTransport::try_send_batch(const ActorAddress& target,
                                                 const StreamBuffer& encoded) {
    FAULT_INJECT("hpactor.transport.batch_send") {}
    auto pool = get_or_create_pool(target.endpoint);
    if (!pool) {
        return TransportSendResult::NotConnected;
    }
    return pool->try_send(target, encoded);
}

bool TcpTransport::is_connected(EndPoint remote_endpoint) const {
    auto it = pools_.find(remote_endpoint);
    if (it != pools_.end()) {
        return it->second->is_connected();
    }
    return false;
}

void TcpTransport::close_connection(EndPoint remote_endpoint) {
    FAULT_INJECT("hpactor.transport.connection.reset") {
        return; // pretend to close but don't
    }
    auto it = pools_.find(remote_endpoint);
    if (it != pools_.end()) {
        it->second->abort();
        pools_.erase(it);
    }
}

void TcpTransport::set_rpc_handler(rpc_response_handler handler) {
    // Store handler and apply to all existing pools
    rpc_handler_ = std::move(handler);
    for (auto& [ep, pool] : pools_) {
        pool->set_rpc_handler(rpc_handler_);
    }
}

void TcpTransport::set_inbound_frame_sink(InboundFrameSink sink) {
    inbound_sink_ = sink;
    for (auto& [ep, pool] : pools_) {
        pool->set_inbound_frame_sink(inbound_sink_);
    }
}

void TcpTransport::set_metrics_ring_buffer(
    metrics::MpscRingBuffer<metrics::MetricEvent>* buf) {
    metrics_ring_buffer_ = buf;
    for (auto& [ep, pool] : pools_) {
        pool->set_metrics_ring_buffer(buf);
    }
}

void TcpTransport::register_connection(ConnectionPtr conn, int fd) {
    connections_[fd] = conn;
}

void TcpTransport::unregister_connection(int fd) {
    connections_.erase(fd);
}

void TcpTransport::handle_accept(int client_fd, EndPoint remote_endpoint) {
    // Get or create pool for the connecting endpoint
    auto pool = get_or_create_pool(remote_endpoint);

    ConnectionPtr conn;
    if (pool_config_.use_tls) {
        auto tls_conn = TlsConnection::create_server(
            client_fd, endpoint_, remote_endpoint, &tls_context_, &loop_);
        tls_conn->set_frame_handler([pool](StreamBuffer data) {
            pool->on_frame_received(std::move(data));
        });
        tls_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        conn = tls_conn;
    } else {
        auto plain_conn = WireFrameConnection::create_as_server(
            client_fd, endpoint_, remote_endpoint, &loop_);
        plain_conn->set_frame_handler([pool](StreamBuffer data) {
            pool->on_frame_received(std::move(data));
        });
        plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
            pool->on_connection_error(c, e);
        });
        plain_conn->set_max_inbound_frame_bytes(pool_config_.max_inbound_frame_bytes);
        conn = plain_conn;
    }

    pool->add_connection(conn);
    register_connection(conn, client_fd);
}

std::string TcpTransport::derive_uds_path(const std::string& node_id) const {
    // Sanitize node_id: replace colons with underscores
    std::string sanitized = node_id;
    for (char& c : sanitized) {
        if (c == ':')
            c = '_';
    }
    return "/tmp/hpactor/" + sanitized + ".sock";
}

} // namespace net
} // namespace hpactor
