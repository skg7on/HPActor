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
#include <hpactor/net/frame.hpp>
#include <functional>

namespace hpactor {

namespace net {

class WireFrameConnection;
using WireFrameConnectionPtr = std::shared_ptr<WireFrameConnection>;

// WireFrameConnection — TCP connection with WireFrame protocol framing.
//
// Reads from the socket accumulate into an internal buffer. When a complete
// WireFrame is available (8-byte header: magic "HPAC" + 4-byte big-endian
// payload length, followed by exactly length payload bytes), the payload is
// delivered as a StreamBuffer to the frame_handler callback.
//
// Wire format expected on the wire:
//   [4 bytes: magic "HPAC"]
//   [4 bytes: remaining_length in network byte order]
//   [N bytes: protobuf-serialized ActorMsgFrame]
class WireFrameConnection : public Connection,
                            public std::enable_shared_from_this<WireFrameConnection> {
  public:
    // Create client-side connection with existing connected fd
    static WireFrameConnectionPtr
    create_as_client(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                     EventLoop* loop);

    // Create server-side connection (from accepted socket)
    static WireFrameConnectionPtr
    create_as_server(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                     EventLoop* loop);

    ~WireFrameConnection();

    // Non-copyable
    WireFrameConnection(const WireFrameConnection&) = delete;
    WireFrameConnection& operator=(const WireFrameConnection&) = delete;

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
    WireFrameConnection(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                        EventLoop* loop);

    // Send raw bytes on socket
    void send_raw(const StreamBuffer& data);

    // Flush write buffer
    void flush_write_buffer();

    // Write buffer initial capacity
    static constexpr size_t kWriteChunkSize = 65536;

    // Accumulation buffer for incoming bytes
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
