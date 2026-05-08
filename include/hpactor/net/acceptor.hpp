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

#include <hpactor/net/event_loop.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// AcceptorInfo - information about a server acceptor
// -----------------------------------------------------------------------------
struct AcceptorInfo {
    uint16_t port = 0;
    uint8_t handshake_version = 0;
    uint8_t protocol_version = 0;
    bool tls_required = false;
};

// -----------------------------------------------------------------------------
// Acceptor - abstract base for server socket listeners
// -----------------------------------------------------------------------------
class Acceptor {
  public:
    using accept_handler =
        std::function<void(int client_fd, EndPoint /*remote_endpoint_hint*/)>;

    explicit Acceptor(EventLoop* loop);
    virtual ~Acceptor();

    // Non-copyable
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    // Stop listening and close the socket
    virtual void close();

    // Set handler for accepted connections
    void set_accept_handler(accept_handler handler);

    // Check if listening
    bool is_listening() const {
        return listening_fd_ >= 0;
    }

  protected:
    virtual void handle_read() = 0;

    EventLoop* loop_;
    int listening_fd_ = -1;
    accept_handler accept_handler_;
};

// -----------------------------------------------------------------------------
// TcpAcceptor - TCP socket acceptor
// -----------------------------------------------------------------------------
class TcpAcceptor : public Acceptor {
  public:
    using Acceptor::Acceptor;

    // Start listening on the specified port.
    // bind_address: IPv4 address to bind to (default "0.0.0.0" = INADDR_ANY).
    // Returns true on success, false on failure.
    bool listen(uint16_t port, uint16_t port_range = 0,
                const std::string& bind_address = "0.0.0.0");

    // Get the bound port
    uint16_t port() const {
        return bound_port_;
    }

  protected:
    void handle_read() override;

  private:
    uint16_t bound_port_ = 0;
};

// -----------------------------------------------------------------------------
// UnixDomainAcceptor - Unix domain socket acceptor
// -----------------------------------------------------------------------------
class UnixDomainAcceptor : public Acceptor {
  public:
    using Acceptor::Acceptor;

    // Start listening on a UNIX domain socket
    // Returns true on success, false on failure
    bool listen(const std::string& path);

    // Get UDS socket path if listening on UDS
    std::string uds_path() const {
        return uds_path_;
    }

    // Stop listening, unlink socket file, and close the fd
    void close() override;

  protected:
    void handle_read() override;

  private:
    std::string uds_path_;
};

} // namespace net
} // namespace hpactor
