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

#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <string>
#include <utility>
#include <vector>

namespace hpactor {
namespace net {

// ---------------------------------------------------------------------------
// HttpSerializer — content negotiation for HTTP ↔ actor messaging
// ---------------------------------------------------------------------------
// Bridges between HTTP wire formats (JSON, protobuf, text) and TypedMessage.
// Determines serialization format based on Content-Type (ingress) and
// Accept (egress) headers.
// ---------------------------------------------------------------------------
class HttpSerializer {
  public:
    HttpSerializer() = default;

    // -----------------------------------------------------------------------
    // Ingress: HTTP request body → TypedMessage
    // -----------------------------------------------------------------------
    // Determines encoding from the request's Content-Type header and converts
    // the body to a TypedMessage with the expected TypeTag. Falls back to
    // treating the body as raw protobuf if Content-Type is absent.
    result<TypedMessage>
    deserialize_request(const HttpRequest& req, TypeTag expected_tag);

    // -----------------------------------------------------------------------
    // Egress: TypedMessage → HTTP response body + Content-Type
    // -----------------------------------------------------------------------
    // Serializes the TypedMessage payload according to the client's Accept
    // header. Returns the response body bytes and the Content-Type string
    // to set on the HTTP response.
    std::pair<StreamBuffer, std::string>
    serialize_response(const TypedMessage& msg, const std::string& accept_header);

    // -----------------------------------------------------------------------
    // Egress: TypedMessage → HTTP request body + Content-Type (for HttpClient)
    // -----------------------------------------------------------------------
    std::pair<StreamBuffer, std::string> serialize_request(const TypedMessage& msg);

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    // Register that a TypeTag should use JSON serialization by default.
    // Without this, the default is protobuf binary.
    void set_default_format(TypeTag tag, const std::string& content_type) {
        default_formats_[static_cast<uint32_t>(tag)] = content_type;
    }

  private:
    // Parse Accept header into ordered list of (media_type, quality) pairs
    struct AcceptedType {
        std::string media_type;
        float quality = 1.0f;
    };
    std::vector<AcceptedType> parse_accept_header(const std::string& header) const;

    // Select the best Content-Type for the response based on Accept header
    std::string negotiate_response_type(const std::string& accept_header) const;

    // Minimal JSON escape/wrap utilities (full JSON↔protobuf mapping is a
    // future phase)
    static StreamBuffer wrap_as_json_bytes(const StreamBuffer& proto_payload);
    static StreamBuffer wrap_as_text_bytes(const StreamBuffer& payload);

    std::unordered_map<uint32_t, std::string> default_formats_;
};

// =============================================================================
// Inline implementations
// =============================================================================

inline result<TypedMessage>
HttpSerializer::deserialize_request(const HttpRequest& req, TypeTag expected_tag) {
    auto ct = req.content_type();

    // Determine content type
    std::string content_type = ct.value_or("application/x-protobuf");

    if (content_type.find("application/x-protobuf") != std::string::npos) {
        // Raw protobuf — pass bytes directly as TypedMessage payload
        return result<TypedMessage>::make(TypedMessage(expected_tag, req.body));
    }

    if (content_type.find("application/json") != std::string::npos) {
        // JSON body — store as-is in TypedMessage; the receiving actor
        // is responsible for JSON→protobuf parsing via as<T>().
        return result<TypedMessage>::make(TypedMessage(expected_tag, req.body));
    }

    if (content_type.find("text/plain") != std::string::npos) {
        // Plain text — wrap as bytes payload
        return result<TypedMessage>::make(TypedMessage(expected_tag, req.body));
    }

    // Unknown Content-Type — pass through as raw bytes
    return result<TypedMessage>::make(TypedMessage(expected_tag, req.body));
}

inline std::pair<StreamBuffer, std::string>
HttpSerializer::serialize_response(const TypedMessage& msg,
                                   const std::string& accept_header) {
    std::string response_type = negotiate_response_type(accept_header);

    if (response_type == "application/x-protobuf") {
        return {msg.payload(), "application/x-protobuf"};
    }

    if (response_type == "text/plain") {
        return {wrap_as_text_bytes(msg.payload()), "text/plain; charset=utf-8"};
    }

    // Default: JSON
    return {wrap_as_json_bytes(msg.payload()), "application/json; "
                                               "charset=utf-8"};
}

inline std::pair<StreamBuffer, std::string>
HttpSerializer::serialize_request(const TypedMessage& msg) {
    // HttpClient always sends protobuf binary for efficiency
    return {msg.payload(), "application/x-protobuf"};
}

inline std::string
HttpSerializer::negotiate_response_type(const std::string& accept_header) const {
    if (accept_header.empty()) {
        return "application/json";
    }

    auto accepted = parse_accept_header(accept_header);

    for (const auto& at : accepted) {
        if (at.media_type == "application/json" ||
            at.media_type == "application/x-protobuf" ||
            at.media_type == "text/plain" || at.media_type == "*/*" ||
            at.media_type == "text/*") {
            // Return the first concrete match, or default to JSON for wildcards
            if (at.media_type == "*/*" || at.media_type == "text/*") {
                return "application/json";
            }
            return at.media_type;
        }
    }

    // No acceptable match — default to JSON
    return "application/json";
}

inline std::vector<HttpSerializer::AcceptedType>
HttpSerializer::parse_accept_header(const std::string& header) const {
    std::vector<AcceptedType> result;

    size_t pos = 0;
    while (pos < header.size()) {
        // Skip whitespace
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) {
            ++pos;
        }

        // Parse media type
        size_t end = header.find_first_of(",;", pos);
        std::string media_type = header.substr(pos, end - pos);
        // Trim trailing whitespace from media type
        while (!media_type.empty() &&
               (media_type.back() == ' ' || media_type.back() == '\t')) {
            media_type.pop_back();
        }
        // Trim leading whitespace
        size_t start = 0;
        while (start < media_type.size() &&
               (media_type[start] == ' ' || media_type[start] == '\t')) {
            ++start;
        }
        media_type = media_type.substr(start);

        pos = end;

        // Parse optional quality parameter
        float quality = 1.0f;
        if (pos < header.size() && header[pos] == ';') {
            ++pos; // skip ';'
            while (pos < header.size() &&
                   (header[pos] == ' ' || header[pos] == '\t')) {
                ++pos;
            }
            if (pos + 2 < header.size() &&
                (header[pos] == 'q' || header[pos] == 'Q') &&
                header[pos + 1] == '=') {
                pos += 2;
                // Parse float
                size_t qend = header.find_first_of(",;", pos);
                std::string qstr = header.substr(
                    pos, qend == std::string::npos ? qend : qend - pos);
                quality = std::stof(qstr);
                pos = qend;
            }
        }

        if (pos < header.size() && header[pos] == ',') {
            ++pos; // skip ','
        }

        result.push_back({std::move(media_type), quality});
    }

    // Sort by quality (descending)
    std::sort(result.begin(), result.end(),
              [](const AcceptedType& a, const AcceptedType& b) {
                  return a.quality > b.quality;
              });

    return result;
}

inline StreamBuffer
HttpSerializer::wrap_as_json_bytes(const StreamBuffer& proto_payload) {
    // Minimal JSON wrapper for protobuf payload.
    // Full JSON↔protobuf conversion is a future phase.
    StreamBuffer result;
    if (proto_payload.size() == 0) {
        const uint8_t empty_json[] = {'{', '}'};
        result.append(empty_json, 2);
        return result;
    }

    static const char* prefix = "{\"data\":\"";
    result.append(reinterpret_cast<const uint8_t*>(prefix), 9);

    // Simple hex encoding for now (future: proper base64/JSON)
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < proto_payload.size() && i < 256; ++i) {
        uint8_t b = proto_payload.data()[i];
        char buf[2] = {hex[b >> 4], hex[b & 0xf]};
        result.append(reinterpret_cast<const uint8_t*>(buf), 2);
    }
    const uint8_t suffix[] = {'"', '}'};
    result.append(suffix, 2);
    return result;
}

inline StreamBuffer
HttpSerializer::wrap_as_text_bytes(const StreamBuffer& payload) {
    if (payload.size() == 0)
        return {};
    return payload;
}

} // namespace net
} // namespace hpactor
