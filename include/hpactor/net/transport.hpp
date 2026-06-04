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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/mailbox/delivery_result.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/rpc/rpc_types.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>

namespace hpactor {

namespace net {

/// \brief Error codes for network transport operations.
enum class TransportError {
    Success = 0,             ///< Operation completed successfully.
    ConnectionFailed = 1,    ///< Connection attempt failed.
    Timeout = 2,             ///< Operation timed out.
    SerializationFailed = 3, ///< Message serialization error.
    BufferOverflow = 4,      ///< Send buffer capacity exceeded.
    NotConnected = 5,        ///< No active connection to the target.
};

/// \brief States of a network connection lifecycle.
enum class ConnectionState {
    Disconnected = 0, ///< No connection established.
    Connecting = 1,   ///< Connection in progress.
    Handshake = 2,    ///< TLS/protocol handshake in progress.
    Connected = 3,    ///< Connection established and ready.
    Error = 4,        ///< Connection is in an error state.
};

// Forward declarations
class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;
class EventLoop;

/// \brief Callback invoked when a connection becomes ready.
using connection_ready_handler = std::function<void(ConnectionPtr)>;

/// \brief Callback for incoming frames.
///
/// Receives a complete \c WireFrame envelope (magic + length header +
/// protobuf payload) as an owning \c StreamBuffer.
using frame_handler = std::function<void(StreamBuffer)>;

/// \brief Callback for connection errors.
using connection_error_handler = std::function<void(ConnectionPtr, const error&)>;

/// \brief Abstract network connection.
///
/// Owns the socket file descriptor, local/remote endpoints, and event loop
/// reference. Derived classes implement protocol-specific read/framing
/// via \c handle_read().
///
/// \note Thread safety: \c send(), \c close(), and \c handle_read() are
///       called from the event loop thread. State transitions use
///       \c set_state() which is not internally synchronized.
class Connection : public std::enable_shared_from_this<Connection> {
  public:
    /// \brief Construct a connection.
    ///
    /// \param[in] fd Socket file descriptor (must be non-blocking for async
    /// I/O).
    /// \param[in] local_endpoint Local endpoint of this connection.
    /// \param[in] remote_endpoint Remote endpoint.
    /// \param[in] loop Owning \c EventLoop.
    Connection(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
               EventLoop* loop);
    virtual ~Connection();

    /// \brief Socket file descriptor.
    int fd() const {
        return fd_;
    }
    /// \brief Local endpoint address.
    EndPoint local_endpoint() const {
        return local_endpoint_;
    }
    /// \brief Remote endpoint address.
    EndPoint remote_endpoint() const {
        return remote_endpoint_;
    }
    /// \brief Owning event loop.
    EventLoop* event_loop() const {
        return loop_;
    }
    /// \brief Current connection state.
    ConnectionState state() const {
        return state_;
    }

    /// \brief Send data on this connection.
    ///
    /// \param[in] data Data to transmit.
    virtual void send(const StreamBuffer& data) = 0;

    /// \brief Close this connection.
    virtual void close() = 0;

    /// \brief Protocol-specific read and frame parsing.
    ///
    /// Called by \c EventLoop when the socket file descriptor is readable.
    /// Implementations must handle edge-triggered semantics.
    /// \note Thread safety: Called from the event loop thread.
    virtual void handle_read() = 0;

    /// \brief Handle completion of an async send operation.
    ///
    /// Called by \c EventLoop via the send completion callback.
    /// \param[in] result Result code from the send operation.
    virtual void handle_send_completion(int result);

    /// \brief Transition to a new connection state.
    ///
    /// \param[in] new_state Target state.
    void set_state(ConnectionState new_state);

  protected:
    int fd_ = -1;
    EndPoint local_endpoint_;
    EndPoint remote_endpoint_;
    EventLoop* loop_ = nullptr;
    ConnectionState state_ = ConnectionState::Disconnected;
};

using ConnectionPtr = std::shared_ptr<Connection>;

/// \brief Abstract interface for network communication.
///
/// Provides connection management and message sending for distributed
/// actor communication. Each \c Transport is associated with a specific
/// node and handles all outgoing connections to remote nodes.
///
/// \note Thread safety: Implementations are called from the network thread
///       and must synchronize internal connection state.
class Transport {
  public:
    virtual ~Transport() = default;

    /// \brief Connect to a remote node using explicit host/port.
    ///
    /// Blocking. Returns the connection on success.
    /// \param[in] remote_endpoint Logical endpoint of the remote node.
    /// \param[in] host Hostname or IP address.
    /// \param[in] port TCP port.
    /// \return Connection pointer, or \c nullptr on failure.
    virtual ConnectionPtr
    connect(EndPoint remote_endpoint, const std::string& host, uint16_t port) = 0;

    /// \brief Connect to a remote node using registry lookup.
    ///
    /// Resolves the host via DNS if needed, then connects. Blocking.
    /// \param[in] remote_endpoint Logical endpoint of the remote node.
    /// \return Connection pointer, or \c nullptr on failure.
    virtual ConnectionPtr connect(EndPoint remote_endpoint) = 0;

    /// \brief Start listening for incoming connections (non-blocking).
    ///
    /// \param[in] port TCP port to bind.
    virtual void listen(uint16_t port) = 0;

    /// \brief Stop accepting incoming connections.
    virtual void stop_listening() = 0;

    /// \brief Try to send a message to a remote actor.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] encoded Serialized message payload.
    /// \return \c TransportSendResult describing whether the frame was
    ///         queued for transmission or why it was rejected.
    virtual TransportSendResult
    try_send(const ActorAddress& target, const StreamBuffer& encoded) = 0;

    /// \brief Send a message to a remote actor (fire-and-forget).
    ///
    /// Default implementation calls \c try_send() and discards the result.
    /// \param[in] target Destination actor address.
    /// \param[in] encoded Serialized message payload.
    virtual void send(const ActorAddress& target, const StreamBuffer& encoded) {
        (void)try_send(target, encoded);
    }

    /// \brief Check if connected to a specific remote node.
    ///
    /// \param[in] remote_endpoint Remote node endpoint.
    /// \return \c true if an active connection exists.
    virtual bool is_connected(EndPoint remote_endpoint) const = 0;

    /// \brief This transport's local endpoint.
    virtual EndPoint endpoint() const = 0;

    /// \brief Close the connection to a specific remote node.
    ///
    /// \param[in] remote_endpoint Remote node endpoint.
    virtual void close_connection(EndPoint remote_endpoint) = 0;

    /// \brief RPC response handler signature.
    using rpc_response_handler = std::function<void(const RpcResponseFrame&)>;

    /// \brief Set the handler for incoming RPC response frames.
    ///
    /// Called by \c ActorSystem when creating the RPC channel.
    /// \param[in] handler Callback for each \c RpcResponseFrame.
    virtual void set_rpc_handler(rpc_response_handler handler) = 0;
};

} // namespace net
} // namespace hpactor
