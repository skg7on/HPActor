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
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/net/transport.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace hpactor {
namespace net {

/// \brief Whether the HTTP connection is server-side or client-side.
enum class HTTPConnectionMode {
    Server, ///< Parses incoming HTTP requests, fires request handlers.
    Client, ///< Parses incoming HTTP responses, fires response handlers.
};

class HTTPConnection;
using HTTPConnectionPtr = std::shared_ptr<HTTPConnection>;

/// \brief TCP connection with HTTP/1.1 framing via llhttp.
///
/// Mirrors the \c WireFrameConnection pattern: registers its fd with
/// \c EventLoop, uses a read handler to accumulate bytes and feed them to
/// \c HttpParser. Supports two modes:
/// - \c Server: parses incoming HTTP requests, fires \c request_handler.
/// - \c Client: parses incoming HTTP responses, fires \c response_handler.
///
/// Send operations build HTTP/1.1 wire bytes and use \c async_send through
/// the \c EventLoop's backend, with write buffering and flush-on-completion.
///
/// \note Thread safety: Called from the event loop thread.
class HTTPConnection : public Connection,
                       public std::enable_shared_from_this<HTTPConnection> {
  public:
    /// \brief Create a connection in the given mode.
    ///
    /// Registers fd with the \c EventLoop for read events.
    /// \param[in] fd Socket file descriptor (must be non-blocking).
    /// \param[in] local_endpoint Local address.
    /// \param[in] remote_endpoint Remote address.
    /// \param[in] loop Owning \c EventLoop.
    /// \param[in] mode \c Server or \c Client.
    /// \return Shared pointer to the new connection.
    static HTTPConnectionPtr
    create(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
           EventLoop* loop, HTTPConnectionMode mode);

    ~HTTPConnection();

    /// \name Non-copyable
    /// @{
    HTTPConnection(const HTTPConnection&) = delete;
    HTTPConnection& operator=(const HTTPConnection&) = delete;
    /// @}

    // ── Callbacks ──────────────────────────────────────────────────────

    /// \brief Server-mode callback: invoked when a complete HTTP request is
    /// parsed.
    ///
    /// \param[in] conn The connection (for sending the response).
    /// \param[in] req Parsed HTTP request.
    using RequestHandler = std::function<void(HTTPConnection*, HttpRequest&&)>;
    void set_request_handler(RequestHandler handler) {
        request_handler_ = std::move(handler);
    }

    /// \brief Client-mode callback: invoked when a complete HTTP response is
    /// parsed.
    ///
    /// \param[in] conn The connection.
    /// \param[in] status_code HTTP status code.
    /// \param[in] headers Response headers.
    /// \param[in] body Response body.
    using ResponseHandler =
        std::function<void(HTTPConnection*, int status_code,
                           std::vector<HttpHeader> headers, StreamBuffer body)>;
    void set_response_handler(ResponseHandler handler) {
        response_handler_ = std::move(handler);
    }

    /// \brief Callback invoked on HTTP parse errors.
    ///
    /// \param[in] conn The connection.
    /// \param[in] err Error details.
    using ErrorHandler = std::function<void(HTTPConnection*, const error&)>;
    void set_error_handler(ErrorHandler handler) {
        error_handler_ = std::move(handler);
    }

    // ── Send ───────────────────────────────────────────────────────────

    /// \brief Build an HTTP/1.1 response and send it on the wire.
    ///
    /// \param[in] code HTTP status code.
    /// \param[in] headers Response headers (empty by default).
    /// \param[in] body Response body (empty by default).
    void send_response(HttpStatusCode code, std::vector<HttpHeader> headers = {},
                       StreamBuffer body = {});

    /// \brief Send raw bytes.
    ///
    /// Appends to the write buffer and flushes asynchronously.
    /// \param[in] data Raw bytes to send.
    void send_raw(const StreamBuffer& data);

    /// \brief Send interface inherited from \c Connection.
    ///
    /// \param[in] data Data to transmit.
    void send(const StreamBuffer& data) override;

    // ── Connection lifecycle ───────────────────────────────────────────

    /// \brief Close the connection.
    ///
    /// \post The fd is deregistered from the event loop.
    void close() override;

    /// \brief Event-loop callback when the fd is readable.
    ///
    /// \note Thread safety: Called from the event loop thread.
    void handle_read() override;

    /// \brief Handle completion of an async send.
    ///
    /// \param[in] result Byte count or negative errno.
    /// \note Thread safety: Called from the event loop thread.
    void handle_send_completion(int result) override;

    /// \brief Delegate keep-alive decision to the parser.
    ///
    /// \return \c true if the connection should be kept alive for the next
    ///         request/response.
    bool should_keep_alive() const;

    /// \brief Return the connection mode.
    ///
    /// \return \c Server or \c Client.
    HTTPConnectionMode mode() const {
        return mode_;
    }

  private:
    HTTPConnection(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                   EventLoop* loop, HTTPConnectionMode mode);

    /// \brief Flush the write buffer via async_send.
    void flush_write_buffer();

    static constexpr size_t kWriteChunkSize = 65536;

    std::unique_ptr<HttpParser> parser_;
    adt::StreamBuffer read_buffer_;
    adt::StreamBuffer write_buffer_;
    bool is_sending_ = false;
    HTTPConnectionMode mode_;

    RequestHandler request_handler_;
    ResponseHandler response_handler_;
    ErrorHandler error_handler_;
};

} // namespace net
} // namespace hpactor
