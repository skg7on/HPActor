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

#include <chrono>
#include <functional>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/transport.hpp>
#include <optional>

namespace hpactor {

namespace net {

class WireFrameConnection;
using WireFrameConnectionPtr = std::shared_ptr<WireFrameConnection>;

/// \brief Configuration for the NET-001 protocol handshake.
///
/// When \c enabled is \c false (the default), \c WireFrameConnection behaves
/// exactly as before — no handshake, immediate transition to \c Connected.
/// When \c enabled is \c true, a two-message version and feature negotiation
/// exchange runs after TCP/TLS connect and before any \c WireEnvelope frames.
struct HandshakeConfig {
    /// \brief Enable the protocol handshake (default false for backward
    /// compatibility).
    bool enabled = false;

    /// \brief Lowest protocol version this node supports.
    uint32_t version_min = 1;

    /// \brief Highest protocol version this node supports.
    uint32_t version_max = 1;

    /// \brief Bitmask of \c HandshakeFeature flags this node supports.
    uint64_t feature_flags = 0;

    /// \brief Maximum time to wait for the handshake to complete.
    std::chrono::milliseconds timeout{5000};

    /// \brief Node identity sent in the hello/response for logging and metrics.
    uint64_t node_id = 0;
};

/// \brief TCP connection with WireFrame protocol framing.
///
/// Reads from the socket accumulate into an internal buffer. When a
/// complete \c WireFrame is available (8-byte header: magic \c "HPAC" +
/// 4-byte big-endian payload length, followed by exactly \c length
/// payload bytes), the canonical HPAC frame bytes (header + payload)
/// are delivered as a \c StreamBuffer to the \c frame_handler callback.
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

    /// \brief Set the maximum inbound frame payload bytes.
    ///
    /// Frames declaring a larger payload are rejected before allocation.
    /// \param[in] max_bytes Maximum payload in bytes (0 = unlimited).
    void set_max_inbound_frame_bytes(uint32_t max_bytes);

    /// \brief Set the framing-error callback.
    ///
    /// Invoked when inbound framing fails (oversize, bad magic, etc.).
    /// Receives the error reason and observed byte count; does not retain
    /// the input bytes.
    /// \param[in] handler Error callback (FrameDecodeError, observed bytes).
    void
    set_frame_error_handler(std::function<void(FrameDecodeError, uint32_t)> handler);

    /// \brief Set the protocol handshake configuration.
    ///
    /// For the connecting-client path, call this after
    /// \c create_connecting_client() and before \c setup_after_connect().
    /// For the server path, call this after \c create_as_server() and
    /// before the first \c handle_read() — the state will be adjusted
    /// automatically.
    /// \param[in] config Handshake configuration.
    void set_handshake_config(HandshakeConfig config);

    /// \brief Return the negotiated handshake result for this connection.
    ///
    /// Only meaningful after the handshake completes successfully.
    /// \return The handshake configuration with negotiated values, or
    ///         \c std::nullopt if the handshake is disabled or has not
    ///         completed.
    std::optional<HandshakeConfig> negotiated_handshake() const {
        if (!handshake_complete_)
            return std::nullopt;
        return handshake_config_;
    }

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
    std::function<void(FrameDecodeError, uint32_t)> frame_error_handler_;
    uint32_t max_inbound_frame_bytes_{16U * 1024U * 1024U};

    // ── Handshake state (NET-001) ──────────────────────────────────────
    HandshakeConfig handshake_config_;
    bool handshake_complete_{false};
    bool handshake_hello_sent_{false};

    /// \brief Start the handshake (client: send HandshakeHello frame).
    void start_handshake();

    /// \brief Send a HandshakeResponse frame on this connection.
    void send_handshake_response(const ::hpactor::net::HandshakeHello& hello);

    /// \brief Transition from Handshake to Connected, firing ready_handler_.
    void complete_handshake();
};

} // namespace net
} // namespace hpactor
