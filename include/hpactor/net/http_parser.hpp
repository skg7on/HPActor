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

#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <string>
#include <vector>

#include "llhttp.h"

namespace hpactor {
namespace net {

/// \brief Internal state of the HTTP message parser.
enum class HttpParseState {
    Idle,           ///< No parsing in progress.
    ParsingHeaders, ///< Parsing request/response headers.
    ParsingBody,    ///< Parsing the message body.
    Complete,       ///< Full message parsed successfully.
    Error,          ///< Parse error encountered.
};

/// \brief Selects between HTTP request and response parsing modes.
enum class HttpParserMode {
    Request,  ///< Parse HTTP requests (method, URL, headers, body).
    Response, ///< Parse HTTP responses (status, headers, body).
};

/// \brief Callback fired in \c Response parsing mode.
///
/// \param[in] status_code HTTP status code.
/// \param[in] headers Response headers.
/// \param[in] body Response body.
using ResponseCallback =
    std::function<void(int status_code, const std::vector<HttpHeader>& headers,
                       const StreamBuffer& body)>;

/// \brief Streaming HTTP/1.1 parser wrapping llhttp.
///
/// Accepts byte buffers incrementally via \c execute() and fires
/// callbacks when a complete message is parsed. Supports both request
/// and response parsing modes.
///
/// \note Thread safety: Not thread-safe. Use one parser per connection.
class HttpParser {
  public:
    /// \brief Callback for a complete HTTP request (request mode).
    ///
    /// \param[in] req Fully parsed HTTP request.
    using MessageCallback = std::function<void(HttpRequest&&)>;

    /// \brief Callback for parse errors.
    ///
    /// \param[in] err llhttp error code.
    /// \param[in] reason Human-readable error description.
    using ErrorCallback = std::function<void(llhttp_errno_t, const char*)>;

    /// \brief Construct a parser in the given mode.
    ///
    /// \param[in] mode Whether to parse requests or responses
    ///            (default \c Request).
    explicit HttpParser(HttpParserMode mode = HttpParserMode::Request);
    ~HttpParser();

    /// \name Non-copyable
    /// @{
    HttpParser(const HttpParser&) = delete;
    HttpParser& operator=(const HttpParser&) = delete;
    /// @}

    /// \brief Feed bytes to the parser.
    ///
    /// Call repeatedly as data arrives. The parser will fire callbacks
    /// when a complete message is available.
    /// \param[in] data Incoming bytes.
    /// \return Number of bytes consumed.
    size_t execute(const StreamBuffer& data);

    /// \brief Set the callback for complete HTTP requests (request mode).
    ///
    /// \param[in] cb Callback invoked with the parsed \c HttpRequest.
    void set_on_message(MessageCallback cb) {
        on_message_ = std::move(cb);
    }

    /// \brief Set the callback for complete HTTP responses (response mode).
    ///
    /// \param[in] cb Callback invoked with status, headers, and body.
    void set_on_response(ResponseCallback cb) {
        on_response_ = std::move(cb);
    }

    /// \brief Set the callback for parse errors.
    ///
    /// \param[in] cb Callback invoked with the llhttp error code and reason.
    void set_on_error(ErrorCallback cb) {
        on_error_ = std::move(cb);
    }

    /// \brief Reset the parser for a new message on the same connection.
    ///
    /// Clears internal buffers and returns to \c Idle state.
    void reset();

    /// \brief Current parse state.
    ///
    /// \return The parser's internal state.
    HttpParseState state() const {
        return state_;
    }

    /// \brief Whether the parser has detected an HTTP upgrade request.
    ///
    /// \return \c true if the client requested a protocol upgrade.
    bool upgrade_requested() const {
        return upgrade_;
    }

    /// \brief Whether the connection should be kept alive.
    ///
    /// \return \c true if \c Connection: keep-alive is set and HTTP/1.1
    ///         semantics allow persistence.
    bool should_keep_alive() const;

  private:
    static int on_message_begin_cb(llhttp_t* parser);
    static int on_url_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_method_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_header_field_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_header_value_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_headers_complete_cb(llhttp_t* parser);
    static int on_body_cb(llhttp_t* parser, const char* data, size_t len);
    static int on_message_complete_cb(llhttp_t* parser);

    void finish_header();

    llhttp_t parser_;
    llhttp_settings_t settings_;
    HttpParserMode mode_ = HttpParserMode::Request;
    HttpParseState state_ = HttpParseState::Idle;
    bool upgrade_ = false;

    // Accumulation buffers
    std::string url_buf_;
    std::string header_name_buf_;
    std::string header_value_buf_;
    std::vector<HttpHeader> headers_;
    HttpMethod method_ = HttpMethod::GET;
    int http_major_ = 1;
    int http_minor_ = 1;
    StreamBuffer body_buf_;

    MessageCallback on_message_;
    ResponseCallback on_response_;
    ErrorCallback on_error_;
};

} // namespace net
} // namespace hpactor
