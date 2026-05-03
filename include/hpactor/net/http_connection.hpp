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

enum class HTTPConnectionMode { Server, Client };

class HTTPConnection;
using HTTPConnectionPtr = std::shared_ptr<HTTPConnection>;

// HTTPConnection — TCP connection with HTTP/1.1 framing via llhttp.
//
// Mirrors the WireFrameConnection pattern: registers its fd with EventLoop,
// uses a read handler to accumulate bytes and feed them to HttpParser.
// Supports two modes:
//   - Server: parses incoming HTTP requests, fires request_handler
//   - Client: parses incoming HTTP responses, fires response_handler
//
// Send operations build HTTP/1.1 wire bytes and use async_send through
// the EventLoop's backend, with write buffering and flush-on-completion.
class HTTPConnection : public Connection,
                       public std::enable_shared_from_this<HTTPConnection> {
  public:
    // Create a connection in the given mode.
    // Registers fd with the EventLoop for read events.
    static HTTPConnectionPtr
    create(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
           EventLoop* loop, HTTPConnectionMode mode);

    ~HTTPConnection();

    // Non-copyable
    HTTPConnection(const HTTPConnection&) = delete;
    HTTPConnection& operator=(const HTTPConnection&) = delete;

    // ---- Callbacks ----

    // Server mode: called when a complete HTTP request is parsed.
    // First argument is the connection itself (for send_response).
    using RequestHandler = std::function<void(HTTPConnection*, HttpRequest&&)>;
    void set_request_handler(RequestHandler handler) {
        request_handler_ = std::move(handler);
    }

    // Client mode: called when a complete HTTP response is parsed.
    using ResponseHandler =
        std::function<void(HTTPConnection*, int status_code,
                           std::vector<HttpHeader> headers, StreamBuffer body)>;
    void set_response_handler(ResponseHandler handler) {
        response_handler_ = std::move(handler);
    }

    // Called on HTTP parse errors.
    using ErrorHandler = std::function<void(HTTPConnection*, const error&)>;
    void set_error_handler(ErrorHandler handler) {
        error_handler_ = std::move(handler);
    }

    // ---- Send ----

    // Build an HTTP/1.1 response and send it on the wire.
    void send_response(HttpStatusCode code,
                       std::vector<HttpHeader> headers = {},
                       StreamBuffer body = {});

    // Send raw bytes (appends to write buffer, flushes asynchronously).
    void send_raw(const StreamBuffer& data);

    // Send interface inherited from Connection.
    void send(const StreamBuffer& data) override;

    // ---- Connection lifecycle ----

    void close() override;

    // Called by the event loop when fd is readable.
    void handle_read() override;

    // Handle send completion (called by EventLoop).
    void handle_send_completion(int result) override;

    // Delegate keep-alive decision to the parser.
    bool should_keep_alive() const;

    // Return the connection mode.
    HTTPConnectionMode mode() const { return mode_; }

  private:
    HTTPConnection(int fd, EndPoint local_endpoint, EndPoint remote_endpoint,
                   EventLoop* loop, HTTPConnectionMode mode);

    // Flush the write buffer via async_send.
    void flush_write_buffer();

    // Write buffer initial capacity.
    static constexpr size_t kWriteChunkSize = 65536;

    // HTTP/1.1 parser (owns llhttp instance).
    std::unique_ptr<HttpParser> parser_;

    // Accumulation buffer for incoming bytes.
    adt::StreamBuffer read_buffer_;

    // Write buffer.
    adt::StreamBuffer write_buffer_;

    // True while async send is in progress.
    bool is_sending_ = false;

    // Connection mode.
    HTTPConnectionMode mode_;

    // Callbacks.
    RequestHandler request_handler_;
    ResponseHandler response_handler_;
    ErrorHandler error_handler_;
};

} // namespace net
} // namespace hpactor
