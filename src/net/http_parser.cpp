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

#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace hpactor {
namespace net {

// =============================================================================
// HttpParser Implementation
// =============================================================================

HttpParser::HttpParser() {
    llhttp_settings_init(&settings_);

    settings_.on_message_begin = on_message_begin_cb;
    settings_.on_url = on_url_cb;
    settings_.on_method = on_method_cb;
    settings_.on_header_field = on_header_field_cb;
    settings_.on_header_value = on_header_value_cb;
    settings_.on_headers_complete = on_headers_complete_cb;
    settings_.on_body = on_body_cb;
    settings_.on_message_complete = on_message_complete_cb;

    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

HttpParser::~HttpParser() = default;

void HttpParser::reset() {
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;

    url_buf_.clear();
    header_name_buf_.clear();
    header_value_buf_.clear();
    headers_.clear();
    body_buf_.clear();
    method_ = HttpMethod::GET;
    http_major_ = 1;
    http_minor_ = 1;
    upgrade_ = false;
    state_ = HttpParseState::Idle;
}

size_t HttpParser::execute(std::span<const uint8_t> data) {
    if (state_ == HttpParseState::Error) return 0;

    auto result = llhttp_execute(&parser_,
                                 reinterpret_cast<const char*>(data.data()),
                                 data.size());

    if (result == HPE_PAUSED) {
        state_ = HttpParseState::ParsingBody;
    } else if (result != HPE_OK) {
        state_ = HttpParseState::Error;
        if (on_error_) {
            on_error_(result, llhttp_errno_name(result));
        }
        return 0;
    }

    return data.size();
}

bool HttpParser::should_keep_alive() const {
    return llhttp_should_keep_alive(&parser_);
}

void HttpParser::finish_header() {
    headers_.push_back({std::move(header_name_buf_), std::move(header_value_buf_)});
    header_name_buf_.clear();
    header_value_buf_.clear();
}

// =============================================================================
// llhttp Callback Trampolines
// =============================================================================

int HttpParser::on_message_begin_cb(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->state_ = HttpParseState::ParsingHeaders;
    self->url_buf_.clear();
    self->headers_.clear();
    self->body_buf_.clear();
    return 0;
}

int HttpParser::on_url_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->url_buf_.append(data, len);
    return 0;
}

int HttpParser::on_method_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->method_ = method_from_string(data, len);
    return 0;
}

int HttpParser::on_header_field_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (!self->header_value_buf_.empty()) {
        self->finish_header();
    }
    self->header_name_buf_.append(data, len);
    return 0;
}

int HttpParser::on_header_value_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->header_value_buf_.append(data, len);
    return 0;
}

int HttpParser::on_headers_complete_cb(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (!self->header_name_buf_.empty()) {
        self->finish_header();
    }
    self->http_major_ = parser->http_major;
    self->http_minor_ = parser->http_minor;
    self->upgrade_ = parser->upgrade;
    return 0;
}

int HttpParser::on_body_cb(llhttp_t* parser, const char* data, size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->body_buf_.append(reinterpret_cast<const uint8_t*>(data), len);
    return 0;
}

int HttpParser::on_message_complete_cb(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->state_ = HttpParseState::Complete;

    if (self->on_message_) {
        HttpRequest req;
        req.method = self->method_;
        req.path = std::move(self->url_buf_);
        req.headers = std::move(self->headers_);
        req.http_major = self->http_major_;
        req.http_minor = self->http_minor_;
        req.body = std::move(self->body_buf_);
        self->on_message_(std::move(req));
    }

    return 0;
}

} // namespace net
} // namespace hpactor
