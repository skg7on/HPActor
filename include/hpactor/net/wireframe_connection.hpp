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

#include <functional>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/transport.hpp>

namespace hpactor {

namespace net {

class WireFrameConnection;
using WireFrameConnectionPtr = std::shared_ptr<WireFrameConnection>;

/// \brief TCP connection with WireFrame protocol framing.
///
/// Reads from the socket accumulate into an internal buffer. When a
/// complete \c WireFrame is available (8-byte header: magic \c "HPAC" +
/// 4-byte big-endian payload length, followed by exactly \c length
/// payload bytes), the payload is delivered as a \c StreamBuffer to
/// the \c frame_handler callback.
///
/// Wire format expected on the wire:
/// \code
///   [4 bytes: magic "HPAC"]
///   [4 bytes: remaining_length in network byte order]
///   [N bytes: protobuf-serialized ActorMsgFrame]
/// \endcode
///
/// \note Thread safety: Called from the event loop thread.
class WireFrameConnection
    : public Connection,
      public std::enable_shared_from_this<WireFrameConnection> {
  public:
    /// \brief Create a client-side connection with an existing connected fd.
    ///
    /// Sets state to \c Connected and registers for Read events.
    /// \param[in] fd Connected socket file descriptor.
    /// \param[in] local_endpoint Local address.
    /// \param[in] remote_endpoint Remote address.
    /// \param[in] loop Owning event loop.
    /// \return Shared pointer to the new connection.
    static WireFrameConnectionPtr
    create_as_client(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                     EventLoop* loop);

    /// \brief Create a client-side connection for a non-blocking connect
    /// in progress.
    ///
    /// Sets state to \c Connecting and does NOT register with the event
    /// loop — the caller must complete the connect and then call
    /// \c setup_after_connect().
    /// \param[in] fd Connecting socket file descriptor.
    /// \param[in] local_endpoint Local address.
    /// \param[in] remote_endpoint Remote address.
    /// \param[in] loop Owning event loop.
    /// \return Shared pointer to the new connection.
    static WireFrameConnectionPtr
    create_connecting_client(int fd, EndPoint local_endpoint,
                             EndPoint remote_endpoint, EventLoop* loop);

    /// \brief Create a server-side connection from an accepted socket.
    ///
    /// \param[in] fd Accepted client file descriptor.
    /// \param[in] local_endpoint Server address.
    /// \param[in] remote_endpoint Client address.
    /// \param[in] loop Owning event loop.
    /// \return Shared pointer to the new connection.
    static WireFrameConnectionPtr
    create_as_server(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                     EventLoop* loop);

    /// \brief Complete post-connect setup.
    ///
    /// Transitions to \c Connected, registers for Read events,
    /// establishes the read handler, and fires the ready handler.
    /// Static to avoid \c shared_from_this issues with dual
    /// \c enable_shared_from_this.
    /// \param[in] conn The connection to set up.
    static void setup_after_connect(WireFrameConnectionPtr conn);

    ~WireFrameConnection();

    /// \name Non-copyable
    /// @{
    WireFrameConnection(const WireFrameConnection&) = delete;
    WireFrameConnection& operator=(const WireFrameConnection&) = delete;
    /// @}

    /// \brief Set the connection-ready callback.
    ///
    /// \param[in] handler Invoked after connect completes.
    void set_ready_handler(std::function<void(ConnectionPtr)> handler);

    /// \brief Set the frame handler for incoming frames.
    ///
    /// \param[in] handler Invoked for each complete wire frame.
    void set_frame_handler(frame_handler handler);

    /// \brief Set the error handler.
    ///
    /// \param[in] handler Invoked on connection errors.
    void
    set_error_handler(std::function<void(ConnectionPtr, const error&)> handler);

    /// \brief Set the send-completion handler.
    ///
    /// \param[in] handler Invoked when an async send completes.
    void set_send_completion_handler(std::function<void(int result)> handler);

    /// \brief Send raw frame data.
    ///
    /// \param[in] frame_data Pre-framed data to send.
    void send(const StreamBuffer& frame_data) override;

    /// \brief Close the connection.
    void close() override;

    /// \brief Event-loop callback when the fd is readable.
    ///
    /// \note Thread safety: Called from the event loop thread.
    void handle_read() override;

    /// \brief Handle async send completion.
    ///
    /// \param[in] result Byte count or negative errno.
    /// \note Thread safety: Called from the event loop thread.
    void handle_send_completion(int result) override;

  private:
    WireFrameConnection(int fd, EndPoint local_endpoint,
                        EndPoint remote_endpoint, EventLoop* loop);

    void send_raw(const StreamBuffer& data);
    void flush_write_buffer();

    static constexpr size_t kWriteChunkSize = 65536;

    adt::StreamBuffer read_buffer_;
    adt::StreamBuffer write_buffer_;
    bool is_sending_ = false;

    std::function<void(ConnectionPtr)> ready_handler_;
    frame_handler frame_handler_;
    std::function<void(ConnectionPtr, const error&)> error_handler_;
    std::function<void(int result)> send_completion_handler_;
};

} // namespace net
} // namespace hpactor
