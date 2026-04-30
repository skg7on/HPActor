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

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace net {

// ---------------------------------------------------------------------------
// HttpMethod
// ---------------------------------------------------------------------------
enum class HttpMethod : uint8_t {
    GET = 0,
    POST = 1,
    PUT = 2,
    DELETE = 3,
    PATCH = 4,
    HEAD = 5,
    OPTIONS = 6,
};

constexpr const char* to_string(HttpMethod method) {
    switch (method) {
    case HttpMethod::GET:     return "GET";
    case HttpMethod::POST:    return "POST";
    case HttpMethod::PUT:     return "PUT";
    case HttpMethod::DELETE:  return "DELETE";
    case HttpMethod::PATCH:   return "PATCH";
    case HttpMethod::HEAD:    return "HEAD";
    case HttpMethod::OPTIONS: return "OPTIONS";
    }
    return "UNKNOWN";
}

constexpr HttpMethod method_from_string(const char* data, size_t len) {
    if (len == 3 && data[0] == 'G' && data[1] == 'E' && data[2] == 'T') return HttpMethod::GET;
    if (len == 3 && data[0] == 'P' && data[1] == 'U' && data[2] == 'T') return HttpMethod::PUT;
    if (len == 4 && data[0] == 'P' && data[1] == 'O' && data[2] == 'S' && data[3] == 'T') return HttpMethod::POST;
    if (len == 4 && data[0] == 'H' && data[1] == 'E' && data[2] == 'A' && data[3] == 'D') return HttpMethod::HEAD;
    if (len == 5 && data[0] == 'P' && data[1] == 'A' && data[2] == 'T' && data[3] == 'C' && data[4] == 'H') return HttpMethod::PATCH;
    if (len == 6 && data[0] == 'D' && data[1] == 'E' && data[2] == 'L' && data[3] == 'E' && data[4] == 'T' && data[5] == 'E') return HttpMethod::DELETE;
    if (len == 7 && data[0] == 'O' && data[1] == 'P' && data[2] == 'T' && data[3] == 'I' && data[4] == 'O' && data[5] == 'N' && data[6] == 'S') return HttpMethod::OPTIONS;
    return HttpMethod::GET;
}

// ---------------------------------------------------------------------------
// HttpStatusCode
// ---------------------------------------------------------------------------
enum class HttpStatusCode : uint16_t {
    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    Conflict = 409,
    PayloadTooLarge = 413,
    UnsupportedMedia = 415,
    TooManyRequests = 429,
    InternalError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
};

inline const char* reason_phrase(HttpStatusCode code) {
    switch (code) {
    case HttpStatusCode::OK:                  return "OK";
    case HttpStatusCode::Created:             return "Created";
    case HttpStatusCode::Accepted:            return "Accepted";
    case HttpStatusCode::NoContent:           return "No Content";
    case HttpStatusCode::BadRequest:          return "Bad Request";
    case HttpStatusCode::Unauthorized:        return "Unauthorized";
    case HttpStatusCode::Forbidden:           return "Forbidden";
    case HttpStatusCode::NotFound:            return "Not Found";
    case HttpStatusCode::MethodNotAllowed:    return "Method Not Allowed";
    case HttpStatusCode::Conflict:            return "Conflict";
    case HttpStatusCode::PayloadTooLarge:     return "Payload Too Large";
    case HttpStatusCode::UnsupportedMedia:    return "Unsupported Media Type";
    case HttpStatusCode::TooManyRequests:     return "Too Many Requests";
    case HttpStatusCode::InternalError:       return "Internal Server Error";
    case HttpStatusCode::NotImplemented:      return "Not Implemented";
    case HttpStatusCode::BadGateway:          return "Bad Gateway";
    case HttpStatusCode::ServiceUnavailable:  return "Service Unavailable";
    case HttpStatusCode::GatewayTimeout:      return "Gateway Timeout";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// HttpHeader
// ---------------------------------------------------------------------------
struct HttpHeader {
    std::string name;   // Lowercase
    std::string value;
};

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------
struct HttpRequest {
    HttpMethod method = HttpMethod::GET;
    std::string path;
    int http_major = 1;
    int http_minor = 1;
    std::vector<HttpHeader> headers;
    bytes body;
    std::unordered_map<std::string, std::string> path_params;
    std::unordered_map<std::string, std::string> query_params;

    std::optional<std::string> header(const std::string& name) const {
        for (const auto& h : headers) {
            if (h.name == name) return h.value;
        }
        return std::nullopt;
    }

    std::optional<std::string> content_type() const {
        return header("content-type");
    }
};

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------
struct HttpResponse {
    HttpStatusCode status_code = HttpStatusCode::OK;
    std::vector<HttpHeader> headers;
    bytes body;

    static HttpResponse ok(bytes body = {}) {
        return {HttpStatusCode::OK, {}, std::move(body)};
    }
    static HttpResponse created(bytes body = {}) {
        return {HttpStatusCode::Created, {}, std::move(body)};
    }
    static HttpResponse no_content() {
        return {HttpStatusCode::NoContent, {}, {}};
    }
    static HttpResponse not_found(bytes body = {}) {
        return {HttpStatusCode::NotFound, {}, std::move(body)};
    }
    static HttpResponse error(HttpStatusCode code, std::string message) {
        bytes body;
        body.append(reinterpret_cast<const uint8_t*>(message.data()), message.size());
        return {code, {{"Content-Type", "text/plain"}}, std::move(body)};
    }
};

} // namespace net
} // namespace hpactor
