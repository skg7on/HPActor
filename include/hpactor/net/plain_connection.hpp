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
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/transport.hpp>

#include <functional>
#include <span>

namespace hpactor {

namespace net {

// Connection pointer type (declared before class for use in factory methods)
class PlainConnection;
using PlainConnectionPtr = std::shared_ptr<PlainConnection>;

class PlainConnection : public Connection,
                        public std::enable_shared_from_this<PlainConnection> {
  public:
    // Create client-side connection with existing connected fd
    static PlainConnectionPtr
    create_client(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                  EventLoop* loop);

    // Create server-side connection (from accepted socket)
    static PlainConnectionPtr
    create_server(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                  EventLoop* loop);

    ~PlainConnection();

    // Non-copyable
    PlainConnection(const PlainConnection&) = delete;
    PlainConnection& operator=(const PlainConnection&) = delete;

    // Set callbacks
    void set_ready_handler(std::function<void(ConnectionPtr)> handler);
    void set_frame_handler(frame_handler handler);
    void
    set_error_handler(std::function<void(ConnectionPtr, const error&)> handler);
    void set_send_completion_handler(std::function<void(int result)> handler);

    // Send raw frame data
    void send(const StreamBuffer& frame_data) override;

    // Close connection
    void close() override;

    // Called by the event loop when fd is readable
    void handle_read() override;

    // Handle send completion (called by EventLoop)
    void handle_send_completion(int result) override;

  private:
    PlainConnection(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                    EventLoop* loop);

    // Send raw bytes on socket
    void send_raw(const StreamBuffer& data);

    // Flush write buffer
    void flush_write_buffer();

    // Read chunk size for ::read() into read_buffer_
    static constexpr size_t kReadChunkSize = 65536;

    // Read buffer
    adt::StreamBuffer read_buffer_;

    // Write buffer
    adt::StreamBuffer write_buffer_;

    // True while async send is in progress
    bool is_sending_ = false;

    // Callbacks
    std::function<void(ConnectionPtr)> ready_handler_;
    frame_handler frame_handler_;
    std::function<void(ConnectionPtr, const error&)> error_handler_;
    std::function<void(int result)> send_completion_handler_;
};

} // namespace net
} // namespace hpactor
