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

#include <hpactor/net/transport.hpp>
#include <hpactor/net/event_loop.hpp>

#include <functional>

namespace hpactor {

namespace net {

// Connection pointer type (declared before class for use in factory methods)
class PlainConnection;
using PlainConnectionPtr = std::shared_ptr<PlainConnection>;

class PlainConnection : public Connection, public std::enable_shared_from_this<PlainConnection> {
public:
    // Create client-side connection with existing connected fd
    static PlainConnectionPtr create_client(int fd,
                                           NodeId remote_node_id,
                                           EventLoop* loop);

    // Create server-side connection (from accepted socket)
    static PlainConnectionPtr create_server(int fd,
                                           NodeId remote_node_id,
                                           EventLoop* loop);

    ~PlainConnection();

    // Non-copyable
    PlainConnection(const PlainConnection&) = delete;
    PlainConnection& operator=(const PlainConnection&) = delete;

    // Getters
    NodeId remote_node_id() const { return remote_node_id_; }
    ConnectionState state() const { return state_; }
    int fd() const { return fd_; }

    // Set callbacks
    void set_ready_handler(connection_ready_handler handler);
    void set_frame_handler(frame_handler handler);
    void set_error_handler(connection_error_handler handler);
    void set_send_completion_handler(std::function<void(int result)> handler);

    // Send raw frame data
    void send(const bytes& frame_data) override;

    // Close connection
    void close() override;

    // Handle incoming data (for framing)
    void handle_read(const bytes& data);

    // Handle send completion (called by EventLoop)
    void handle_send_completion(int result);

private:
    PlainConnection(NodeId remote_node_id, EventLoop* loop, int fd);

    void set_state(ConnectionState new_state);

    // Send raw bytes on socket
    void send_raw(const bytes& data);

    // Flush write buffer
    void flush_write_buffer();

    NodeId remote_node_id_ = 0;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;

    ConnectionState state_ = ConnectionState::Disconnected;

    // Read buffer
    bytes read_buffer_;

    // Write buffer
    bytes write_buffer_;

    // True while async send is in progress
    bool is_sending_ = false;

    // Callbacks
    connection_ready_handler ready_handler_;
    frame_handler frame_handler_;
    connection_error_handler error_handler_;
    std::function<void(int result)> send_completion_handler_;
};

} // namespace net
} // namespace hpactor