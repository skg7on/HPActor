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

// -----------------------------------------------------------------------------
// Connection - represents a connection to a remote node
// -----------------------------------------------------------------------------
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using message_handler = std::function<void(const bytes&)>;

    Connection(NodeId remote_node);
    virtual ~Connection();

    NodeId remote_node() const { return remote_node_; }
    ConnectionState state() const { return state_; }

    // Set handler for incoming messages
    void set_message_handler(message_handler handler);

    // Send data on this connection
    virtual void send(const bytes& data) = 0;

    // Close this connection
    virtual void close() = 0;

    // Handle incoming data (for framing)
    void handle_read(const bytes& data);

    // Transition to a new state
    void set_state(ConnectionState new_state);

protected:
    virtual void on_message(const bytes& data);

private:
    NodeId remote_node_;
    ConnectionState state_ = ConnectionState::Disconnected;
    message_handler message_handler_;
    bytes read_buffer_;  // For partial frame reads
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
    virtual ConnectionPtr connect(NodeId remote_node,
                                const std::string& host,
                                uint16_t port) = 0;

    // Connect to a remote node using registry lookup (DNS resolution if needed)
    // Returns ConnectionPtr on success, nullptr on failure
    virtual ConnectionPtr connect(NodeId remote_node) = 0;

    // Start listening for incoming connections (non-blocking)
    virtual void listen(uint16_t port) = 0;

    // Stop listening
    virtual void stop_listening() = 0;

    // Send a message to a remote actor
    // The encoded parameter contains the serialized message
    virtual void send(const ActorAddress& target, const bytes& encoded) = 0;

    // Check if connected to a specific node
    virtual bool is_connected(NodeId remote_node) const = 0;

    // Get this transport's node ID
    virtual NodeId node_id() const = 0;

    // Close connection to a specific node
    virtual void close_connection(NodeId remote_node) = 0;
};

} // namespace net
} // namespace hpactor
