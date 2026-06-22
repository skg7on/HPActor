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

/// \brief HTTP method enum with constexpr string conversion.
enum class HttpMethod : uint8_t {
    GET = 0,     ///< Retrieve a resource.
    POST = 1,    ///< Create a resource.
    PUT = 2,     ///< Replace a resource.
    DELETE = 3,  ///< Remove a resource.
    PATCH = 4,   ///< Partially update a resource.
    HEAD = 5,    ///< Retrieve headers only.
    OPTIONS = 6, ///< Query supported methods.
};

/// \brief Return the uppercase HTTP method string.
///
/// \param[in] method The method to convert.
/// \return String literal (e.g., \c "GET", \c "POST").
constexpr const char* to_string(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:
            return "GET";
        case HttpMethod::POST:
            return "POST";
        case HttpMethod::PUT:
            return "PUT";
        case HttpMethod::DELETE:
            return "DELETE";
        case HttpMethod::PATCH:
            return "PATCH";
        case HttpMethod::HEAD:
            return "HEAD";
        case HttpMethod::OPTIONS:
            return "OPTIONS";
    }
    return "UNKNOWN";
}

/// \brief Parse an HTTP method from a character buffer.
///
/// \param[in] data Pointer to the method string.
/// \param[in] len Length of the method string.
/// \return Matching \c HttpMethod, or \c HttpMethod::GET if unrecognized.
constexpr HttpMethod method_from_string(const char* data, size_t len) {
    if (len == 3 && data[0] == 'G' && data[1] == 'E' && data[2] == 'T')
        return HttpMethod::GET;
    if (len == 3 && data[0] == 'P' && data[1] == 'U' && data[2] == 'T')
        return HttpMethod::PUT;
    if (len == 4 && data[0] == 'P' && data[1] == 'O' && data[2] == 'S' &&
        data[3] == 'T')
        return HttpMethod::POST;
    if (len == 4 && data[0] == 'H' && data[1] == 'E' && data[2] == 'A' &&
        data[3] == 'D')
        return HttpMethod::HEAD;
    if (len == 5 && data[0] == 'P' && data[1] == 'A' && data[2] == 'T' &&
        data[3] == 'C' && data[4] == 'H')
        return HttpMethod::PATCH;
    if (len == 6 && data[0] == 'D' && data[1] == 'E' && data[2] == 'L' &&
        data[3] == 'E' && data[4] == 'T' && data[5] == 'E')
        return HttpMethod::DELETE;
    if (len == 7 && data[0] == 'O' && data[1] == 'P' && data[2] == 'T' &&
        data[3] == 'I' && data[4] == 'O' && data[5] == 'N' && data[6] == 'S')
        return HttpMethod::OPTIONS;
    return HttpMethod::GET;
}

/// \brief HTTP status codes with reason-phrase lookup.
enum class HttpStatusCode : uint16_t {
    OK = 200,                 ///< 200 OK.
    Created = 201,            ///< 201 Created.
    Accepted = 202,           ///< 202 Accepted.
    NoContent = 204,          ///< 204 No Content.
    BadRequest = 400,         ///< 400 Bad Request.
    Unauthorized = 401,       ///< 401 Unauthorized.
    Forbidden = 403,          ///< 403 Forbidden.
    NotFound = 404,           ///< 404 Not Found.
    MethodNotAllowed = 405,   ///< 405 Method Not Allowed.
    Conflict = 409,           ///< 409 Conflict.
    PayloadTooLarge = 413,    ///< 413 Payload Too Large.
    UnsupportedMedia = 415,   ///< 415 Unsupported Media Type.
    TooManyRequests = 429,    ///< 429 Too Many Requests.
    InternalError = 500,      ///< 500 Internal Server Error.
    NotImplemented = 501,     ///< 501 Not Implemented.
    BadGateway = 502,         ///< 502 Bad Gateway.
    ServiceUnavailable = 503, ///< 503 Service Unavailable.
    GatewayTimeout = 504,     ///< 504 Gateway Timeout.
};

/// \brief Return the standard reason phrase for a status code.
///
/// \param[in] code HTTP status code.
/// \return String literal (e.g., \c "OK", \c "Not Found").
inline const char* reason_phrase(HttpStatusCode code) {
    switch (code) {
        case HttpStatusCode::OK:
            return "OK";
        case HttpStatusCode::Created:
            return "Created";
        case HttpStatusCode::Accepted:
            return "Accepted";
        case HttpStatusCode::NoContent:
            return "No Content";
        case HttpStatusCode::BadRequest:
            return "Bad Request";
        case HttpStatusCode::Unauthorized:
            return "Unauthorized";
        case HttpStatusCode::Forbidden:
            return "Forbidden";
        case HttpStatusCode::NotFound:
            return "Not Found";
        case HttpStatusCode::MethodNotAllowed:
            return "Method Not Allowed";
        case HttpStatusCode::Conflict:
            return "Conflict";
        case HttpStatusCode::PayloadTooLarge:
            return "Payload Too Large";
        case HttpStatusCode::UnsupportedMedia:
            return "Unsupported Media Type";
        case HttpStatusCode::TooManyRequests:
            return "Too Many Requests";
        case HttpStatusCode::InternalError:
            return "Internal Server Error";
        case HttpStatusCode::NotImplemented:
            return "Not Implemented";
        case HttpStatusCode::BadGateway:
            return "Bad Gateway";
        case HttpStatusCode::ServiceUnavailable:
            return "Service Unavailable";
        case HttpStatusCode::GatewayTimeout:
            return "Gateway Timeout";
    }
    return "Unknown";
}

/// \brief A single HTTP header (name, value pair).
struct HttpHeader {
    /// \brief Header name in lowercase.
    std::string name;
    /// \brief Header value.
    std::string value;
};

/// \brief Parsed HTTP request.
///
/// Populated by \c HttpParser during HTTP/1.1 message parsing. Includes
/// method, path, headers, body, and parsed path/query parameters.
struct HttpRequest {
    /// \brief HTTP method.
    HttpMethod method = HttpMethod::GET;
    /// \brief Request path (without query string).
    std::string path;
    /// \brief HTTP major version (typically 1).
    int http_major = 1;
    /// \brief HTTP minor version (typically 0 or 1).
    int http_minor = 1;
    /// \brief Request headers.
    std::vector<HttpHeader> headers;
    /// \brief Request body.
    StreamBuffer body;
    /// \brief Path parameters extracted by \c RouteRegistry::match().
    std::unordered_map<std::string, std::string> path_params;
    /// \brief Query string parameters parsed from the URL.
    std::unordered_map<std::string, std::string> query_params;

    /// \brief Look up a header by name (case-sensitive).
    ///
    /// \param[in] name Header name in lowercase.
    /// \return The header value, or \c std::nullopt if not found.
    std::optional<std::string> header(const std::string& name) const {
        for (const auto& h : headers) {
            if (h.name == name)
                return h.value;
        }
        return std::nullopt;
    }

    /// \brief Shortcut for \c header("content-type").
    ///
    /// \return The Content-Type value, or \c std::nullopt if not set.
    std::optional<std::string> content_type() const {
        return header("content-type");
    }
};

/// \brief HTTP response to be serialized and sent.
///
/// Constructed by route handlers or by the gateway for error responses.
/// Use the static factory methods for common status codes.
struct HttpResponse {
    /// \brief HTTP status code.
    HttpStatusCode status_code = HttpStatusCode::OK;
    /// \brief Response headers.
    std::vector<HttpHeader> headers;
    /// \brief Response body.
    StreamBuffer body;

    /// \brief Build a 200 OK response.
    ///
    /// \param[in] body Response body (empty by default).
    /// \return An \c HttpResponse with status \c OK.
    static HttpResponse ok(StreamBuffer body = {}) {
        return {HttpStatusCode::OK, {}, std::move(body)};
    }
    /// \brief Build a 201 Created response.
    ///
    /// \param[in] body Response body (empty by default).
    /// \return An \c HttpResponse with status \c Created.
    static HttpResponse created(StreamBuffer body = {}) {
        return {HttpStatusCode::Created, {}, std::move(body)};
    }
    /// \brief Build a 204 No Content response (no body).
    ///
    /// \return An \c HttpResponse with status \c NoContent.
    static HttpResponse no_content() {
        return {HttpStatusCode::NoContent, {}, {}};
    }
    /// \brief Build a 404 Not Found response.
    ///
    /// \param[in] body Response body (empty by default).
    /// \return An \c HttpResponse with status \c NotFound.
    static HttpResponse not_found(StreamBuffer body = {}) {
        return {HttpStatusCode::NotFound, {}, std::move(body)};
    }
    /// \brief Build an error response with a plain-text message body.
    ///
    /// \param[in] code HTTP error status code.
    /// \param[in] message Human-readable error message.
    /// \return An \c HttpResponse with the given code and a
    ///         \c Content-Type: \c text/plain body.
    static HttpResponse error(HttpStatusCode code, std::string message) {
        StreamBuffer body;
        body.append(reinterpret_cast<const uint8_t*>(message.data()),
                    message.size());
        return {code, {{"Content-Type", "text/plain"}}, std::move(body)};
    }
};

} // namespace net
} // namespace hpactor
