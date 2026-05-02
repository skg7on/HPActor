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
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// TransportError - error codes for network operations
// -----------------------------------------------------------------------------
enum class TransportError {
    Success = 0,
    ConnectionFailed = 1,
    Timeout = 2,
    SerializationFailed = 3,
    BufferOverflow = 4,
    NotConnected = 5,
};

// -----------------------------------------------------------------------------
// ConnectionState - state of a network connection
// -----------------------------------------------------------------------------
enum class ConnectionState {
    Disconnected = 0,
    Connecting = 1,
    Handshake = 2,
    Connected = 3,
    Error = 4,
};

// Forward declarations
class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;
class EventLoop;

// -----------------------------------------------------------------------------
// Connection callback types
// -----------------------------------------------------------------------------
// Callback for when connection becomes ready
using connection_ready_handler = std::function<void(ConnectionPtr)>;
// Callback for incoming frames — receives a complete WireFrame envelope
// (magic + length header + protobuf payload) as an owning StreamBuffer.
using frame_handler = std::function<void(StreamBuffer)>;
// Callback for connection errors
using connection_error_handler = std::function<void(ConnectionPtr, const error&)>;

// -----------------------------------------------------------------------------
// Connection - represents a connection to a remote node
// -----------------------------------------------------------------------------
// Owns the socket fd, local/remote endpoints, and event loop reference.
// Derived classes implement protocol-specific read/framing via handle_read().
// -----------------------------------------------------------------------------
class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
               EventLoop* loop);
    virtual ~Connection();

    int fd() const { return fd_; }
    EndPoint local_endpoint() const { return local_endpoint_; }
    EndPoint remote_endpoint() const { return remote_endpoint_; }
    EventLoop* event_loop() const { return loop_; }
    ConnectionState state() const { return state_; }

    // Send data on this connection
    virtual void send(const StreamBuffer& data) = 0;

    // Close this connection
    virtual void close() = 0;

    // Protocol-specific read/framing — called when fd is readable
    virtual void handle_read() = 0;

    // Handle send completion (called by EventLoop on async_send completion)
    virtual void handle_send_completion(int result);

    // Transition to a new state
    void set_state(ConnectionState new_state);

  protected:
    int fd_ = -1;
    EndPoint local_endpoint_;
    EndPoint remote_endpoint_;
    EventLoop* loop_ = nullptr;
    ConnectionState state_ = ConnectionState::Disconnected;
};

// Connection pointer type
using ConnectionPtr = std::shared_ptr<Connection>;

// -----------------------------------------------------------------------------
// Transport - abstraction for network communication
// -----------------------------------------------------------------------------
// The Transport interface provides connection management and message sending
// for distributed actor communication. Each Transport is associated with
// a specific node and handles all outgoing connections to remote nodes.
// -----------------------------------------------------------------------------
class Transport {
  public:
    virtual ~Transport() = default;

    // Connect to a remote node using explicit host/port (blocking)
    // Returns a Connection pointer on success, nullptr on failure
    virtual ConnectionPtr connect(EndPoint remote_endpoint,
                                  const std::string& host, uint16_t port) = 0;

    // Connect to a remote node using registry lookup (DNS resolution if needed)
    // Returns ConnectionPtr on success, nullptr on failure
    virtual ConnectionPtr connect(EndPoint remote_endpoint) = 0;

    // Start listening for incoming connections (non-blocking)
    virtual void listen(uint16_t port) = 0;

    // Stop listening
    virtual void stop_listening() = 0;

    // Send a message to a remote actor
    // The encoded parameter contains the serialized message
    virtual void send(const ActorAddress& target, const StreamBuffer& encoded) = 0;

    // Check if connected to a specific node
    virtual bool is_connected(EndPoint remote_endpoint) const = 0;

    // Get this transport's node ID
    virtual EndPoint endpoint() const = 0;

    // Close connection to a specific node
    virtual void close_connection(EndPoint remote_endpoint) = 0;

    // Set RPC response handler - called when RPC response frames are received
    using rpc_response_handler = std::function<void(MessageId, const StreamBuffer&)>;
    virtual void set_rpc_handler(rpc_response_handler handler) = 0;
};

} // namespace net
} // namespace hpactor
