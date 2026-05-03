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

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace net {

// =============================================================================
// URL Parsing
// =============================================================================
struct ParsedUrl {
    std::string scheme;
    std::string host;
    uint16_t port;
    std::string path;
};

// Note: uses std::stoi for port — assumes well-formed numeric ports.
// Validation of malformed URLs (e.g. "http://host:abc/path") is out of scope
// for this phase.
static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl result;

    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        result.scheme = "http";
        scheme_end = 0;
    } else {
        result.scheme = url.substr(0, scheme_end);
        scheme_end += 3;
    }

    auto host_end = url.find_first_of(":/", scheme_end);
    result.host = url.substr(scheme_end, host_end - scheme_end);

    if (host_end != std::string::npos && url[host_end] == ':') {
        auto port_end = url.find('/', host_end);
        auto port_str = url.substr(host_end + 1,
            port_end == std::string::npos ? std::string::npos
                                          : port_end - host_end - 1);
        result.port = static_cast<uint16_t>(std::stoi(port_str));
        host_end = port_end;
    } else {
        result.port = (result.scheme == "https") ? 443 : 80;
    }

    result.path = (host_end != std::string::npos) ? url.substr(host_end) : "/";
    return result;
}

// =============================================================================
// HttpClient Implementation — blocking TCP (for test usage)
// =============================================================================

HttpClient::HttpClient(EventLoop* loop) : loop_(loop) {}

HttpClient::~HttpClient() { abort(); }

RpcFuture<StreamBuffer>
HttpClient::request(HttpMethod method, const std::string& url,
                    std::vector<HttpHeader> headers, StreamBuffer body) {
    auto parsed = parse_url(url);

    // 1. Connect
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::promise<result<StreamBuffer>> p;
        p.set_value(result<StreamBuffer>::make(
            error(errors::http_connect_failed, "socket() failed")));
        return RpcFuture<StreamBuffer>(p.get_future(), default_timeout_);
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(parsed.port);
    inet_pton(AF_INET, parsed.host.c_str(), &addr.sin_addr);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        std::promise<result<StreamBuffer>> p;
        p.set_value(result<StreamBuffer>::make(
            error(errors::http_connect_failed, "connect() failed")));
        return RpcFuture<StreamBuffer>(p.get_future(), default_timeout_);
    }

    // 2. Build HTTP/1.1 request wire bytes
    StreamBuffer wire;
    std::string request_line = to_string(method);
    request_line += " " + parsed.path + " HTTP/1.1\r\n";
    wire.append(reinterpret_cast<const uint8_t*>(request_line.data()),
                request_line.size());

    std::string host_header = "Host: " + parsed.host;
    if (parsed.port != 80) {
        host_header += ":" + std::to_string(parsed.port);
    }
    host_header += "\r\n";
    wire.append(reinterpret_cast<const uint8_t*>(host_header.data()),
                host_header.size());

    for (const auto& h : headers) {
        std::string hdr = h.name + ": " + h.value + "\r\n";
        wire.append(reinterpret_cast<const uint8_t*>(hdr.data()), hdr.size());
    }

    if (!body.empty()) {
        std::string cl =
            "Content-Length: " + std::to_string(body.size()) + "\r\n";
        wire.append(reinterpret_cast<const uint8_t*>(cl.data()), cl.size());
    }
    const uint8_t crlf[] = {'\r', '\n'};
    wire.append(crlf, 2);
    wire.append(body.data(), body.size());

    // 3. Send
    ssize_t sent = write(fd, wire.data(), wire.size());
    if (sent < 0) {
        close(fd);
        std::promise<result<StreamBuffer>> p;
        p.set_value(result<StreamBuffer>::make(
            error(errors::http_connect_failed, "write() failed")));
        return RpcFuture<StreamBuffer>(p.get_future(), default_timeout_);
    }

    // 4. Read response via llhttp in HTTP_RESPONSE mode
    HttpParser parser(HttpParserMode::Response);
    int response_status = 0;
    StreamBuffer response_body;
    bool complete = false;

    parser.set_on_response(
        [&](int status, const std::vector<HttpHeader>& /*resp_headers*/,
            const StreamBuffer& body) {
            response_status = status;
            response_body = body;
            complete = true;
        });

    StreamBuffer read_buf;
    ssize_t n;
    while (!complete && (n = read(fd, read_buf.reserve_tail(4096), 4096)) > 0) {
        read_buf.commit_tail(static_cast<size_t>(n));
        size_t consumed = parser.execute(read_buf);
        if (consumed > 0) {
            read_buf.consume(consumed);
        }
    }
    close(fd);

    // 5. Fulfill promise
    std::promise<result<StreamBuffer>> p;
    if (response_status >= 200 && response_status < 300) {
        p.set_value(result<StreamBuffer>::make(std::move(response_body)));
    } else if (response_status == 0) {
        p.set_value(result<StreamBuffer>::make(
            error(errors::http_parse_error, "No response received")));
    } else {
        p.set_value(result<StreamBuffer>::make(
            error(errors::http_parse_error,
                  "HTTP " + std::to_string(response_status))));
    }
    return RpcFuture<StreamBuffer>(p.get_future(), default_timeout_);
}

RpcFuture<StreamBuffer>
HttpClient::get(const std::string& url, std::vector<HttpHeader> headers) {
    return request(HttpMethod::GET, url, std::move(headers), {});
}

RpcFuture<StreamBuffer>
HttpClient::post(const std::string& url, StreamBuffer body,
                  std::vector<HttpHeader> headers) {
    return request(HttpMethod::POST, url, std::move(headers), std::move(body));
}

RpcFuture<StreamBuffer>
HttpClient::put(const std::string& url, StreamBuffer body,
                 std::vector<HttpHeader> headers) {
    return request(HttpMethod::PUT, url, std::move(headers), std::move(body));
}

RpcFuture<StreamBuffer>
HttpClient::del(const std::string& url, std::vector<HttpHeader> headers) {
    return request(HttpMethod::DELETE, url, std::move(headers), {});
}

void HttpClient::abort() {
    // Full implementation deferred — blocking I/O mode has no in-flight state.
}

} // namespace net
} // namespace hpactor
