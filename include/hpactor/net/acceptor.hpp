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

/// \brief Server acceptor advertisement exchanged during registration.
///
/// Describes a listening socket that a registrar client publishes so other
/// nodes can discover how to connect.
struct AcceptorInfo {
    /// \brief TCP port the acceptor is listening on.
    uint16_t port = 0;
    /// \brief Handshake protocol version.
    uint8_t handshake_version = 0;
    /// \brief Application protocol version.
    uint8_t protocol_version = 0;
    /// \brief Whether TLS is required for connections on this acceptor.
    bool tls_required = false;
};

/// \brief Abstract base for server socket listeners.
///
/// Owns a listening socket file descriptor and an \c EventLoop reference.
/// Derived classes implement protocol-specific accept behavior via
/// \c handle_read().
///
/// \note Thread safety: Called from the event loop thread only.
class Acceptor {
  public:
    /// \brief Callback invoked when a new client connection is accepted.
    ///
    /// \param[in] client_fd Accepted client file descriptor.
    /// \param[in] remote_endpoint_hint Best-effort remote address hint.
    using accept_handler =
        std::function<void(int client_fd, EndPoint /*remote_endpoint_hint*/)>;

    /// \brief Construct an acceptor bound to the given event loop.
    ///
    /// \param[in] loop Owning \c EventLoop. Must outlive this acceptor.
    explicit Acceptor(EventLoop* loop);
    virtual ~Acceptor();

    /// \name Non-copyable
    /// @{
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    /// @}

    /// \brief Stop listening and close the socket.
    ///
    /// \pre The acceptor must be in the listening state.
    virtual void close();

    /// \brief Register a handler for accepted connections.
    ///
    /// \param[in] handler Callback invoked on each accepted connection.
    void set_accept_handler(accept_handler handler);

    /// \brief Check whether the acceptor is listening.
    ///
    /// \return \c true if a valid listening socket is open.
    bool is_listening() const {
        return listening_fd_ >= 0;
    }

  protected:
    /// \brief Event-loop callback invoked when the listening socket is
    /// readable.
    ///
    /// Implementations must call \c ::accept() and invoke
    /// \c accept_handler_ with the new client fd.
    /// \note Thread safety: Called from the event loop thread.
    virtual void handle_read() = 0;

    /// \brief Owning event loop.
    EventLoop* loop_;
    /// \brief Listening socket file descriptor, or -1 if not listening.
    int listening_fd_ = -1;
    /// \brief Registered accept callback.
    accept_handler accept_handler_;
};

/// \brief TCP socket acceptor.
///
/// Binds to an IPv4 address and port, listens for TCP connections, and
/// delivers accepted client fds via the accept handler.
///
/// \note Thread safety: Called from the event loop thread.
class TcpAcceptor : public Acceptor {
  public:
    using Acceptor::Acceptor;

    /// \brief Start listening on the specified TCP port.
    ///
    /// \param[in] port TCP port to bind.
    /// \param[in] port_range If non-zero, scan up to this many ports after
    ///            \c port if the first bind fails.
    /// \param[in] bind_address IPv4 address to bind to
    ///            (default \c "0.0.0.0" = \c INADDR_ANY).
    /// \return \c true on success, \c false on bind failure (all ports
    ///         exhausted).
    bool listen(uint16_t port, uint16_t port_range = 0,
                const std::string& bind_address = "0.0.0.0");

    /// \brief Return the actually-bound port.
    ///
    /// \return TCP port number, or 0 if not yet listening.
    uint16_t port() const {
        return bound_port_;
    }

  protected:
    void handle_read() override;

  private:
    uint16_t bound_port_ = 0;
};

/// \brief UNIX domain socket acceptor.
///
/// Binds to a filesystem path, listens for stream connections, and delivers
/// accepted client fds via the accept handler. The socket file is unlinked
/// on \c close().
///
/// \note Thread safety: Called from the event loop thread.
class UnixDomainAcceptor : public Acceptor {
  public:
    using Acceptor::Acceptor;

    /// \brief Start listening on a UNIX domain socket.
    ///
    /// \param[in] path Filesystem path for the socket file.
    /// \return \c true on success, \c false on bind failure.
    bool listen(const std::string& path);

    /// \brief Return the UDS path if listening.
    ///
    /// \return Socket path, or empty string if not listening.
    std::string uds_path() const {
        return uds_path_;
    }

    /// \brief Stop listening, unlink the socket file, and close the fd.
    ///
    /// \post The socket file is removed from the filesystem.
    void close() override;

  protected:
    void handle_read() override;

  private:
    std::string uds_path_;
};

} // namespace net
} // namespace hpactor
