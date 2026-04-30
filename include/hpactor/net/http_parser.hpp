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
#include <span>
#include <string>
#include <vector>

#include "llhttp.h"

namespace hpactor {
namespace net {

// ---------------------------------------------------------------------------
// HttpParseState
// ---------------------------------------------------------------------------
enum class HttpParseState {
    Idle,
    ParsingHeaders,
    ParsingBody,
    Complete,
    Error,
};

// ---------------------------------------------------------------------------
// HttpParserMode — selects between HTTP request and response parsing
// ---------------------------------------------------------------------------
enum class HttpParserMode { Request, Response };

// ---------------------------------------------------------------------------
// ResponseCallback — fired in Response mode with status, headers, body
// ---------------------------------------------------------------------------
using ResponseCallback = std::function<void(int status_code,
    const std::vector<HttpHeader>& headers, const bytes& body)>;

// ---------------------------------------------------------------------------
// HttpParser — wraps llhttp for use with StreamBuffer and HttpRequest
// ---------------------------------------------------------------------------
class HttpParser {
  public:
    using MessageCallback = std::function<void(HttpRequest&&)>;
    using ErrorCallback = std::function<void(llhttp_errno_t, const char*)>;

    explicit HttpParser(HttpParserMode mode = HttpParserMode::Request);
    ~HttpParser();

    HttpParser(const HttpParser&) = delete;
    HttpParser& operator=(const HttpParser&) = delete;

    size_t execute(std::span<const uint8_t> data);

    void set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
    void set_on_response(ResponseCallback cb) { on_response_ = std::move(cb); }
    void set_on_error(ErrorCallback cb) { on_error_ = std::move(cb); }

    void reset();

    HttpParseState state() const { return state_; }
    bool upgrade_requested() const { return upgrade_; }
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
    bytes body_buf_;

    MessageCallback on_message_;
    ResponseCallback on_response_;
    ErrorCallback on_error_;
};

} // namespace net
} // namespace hpactor
