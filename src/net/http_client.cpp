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
#include <hpactor/net/http_types.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace net {

// =============================================================================
// HttpKeepAlivePool — per-host connection pool
// =============================================================================
class HttpKeepAlivePool {
  public:
    explicit HttpKeepAlivePool(size_t max_conns = 4) : max_conns_(max_conns) {}

    // Placeholder: acquire an idle connection or create a new one
    int acquire(const std::string& /*host*/, uint16_t /*port*/) {
        return -1;
    }

    void release(int /*fd*/, bool /*keep_alive*/) {}

  private:
    [[maybe_unused]] size_t max_conns_ = 4;
    struct PooledConn { int fd = -1; bool in_use = false; };
    std::vector<PooledConn> conns_;
};

// =============================================================================
// URL Parsing
// =============================================================================
struct ParsedUrl {
    std::string scheme;
    std::string host;
    uint16_t port;
    std::string path;
};

[[maybe_unused]] static ParsedUrl parse_url(const std::string& url) {
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
// HttpClient Implementation (stub)
// =============================================================================

HttpClient::HttpClient(EventLoop* loop) : loop_(loop) {}

HttpClient::~HttpClient() { abort(); }

RpcFuture<bytes>
HttpClient::request(HttpMethod /*method*/, const std::string& /*url*/,
                    std::vector<HttpHeader> /*headers*/, bytes /*body*/) {
    // Stub — full implementation in next iteration
    std::promise<result<bytes>> p;
    p.set_value(result<bytes>::make(bytes{}));
    return RpcFuture<bytes>(p.get_future(), std::chrono::milliseconds(5000));
}

RpcFuture<bytes>
HttpClient::get(const std::string& url, std::vector<HttpHeader> headers) {
    return request(HttpMethod::GET, url, std::move(headers), {});
}

RpcFuture<bytes>
HttpClient::post(const std::string& url, bytes body,
                  std::vector<HttpHeader> headers) {
    return request(HttpMethod::POST, url, std::move(headers), std::move(body));
}

RpcFuture<bytes>
HttpClient::put(const std::string& url, bytes body,
                 std::vector<HttpHeader> headers) {
    return request(HttpMethod::PUT, url, std::move(headers), std::move(body));
}

RpcFuture<bytes>
HttpClient::del(const std::string& url, std::vector<HttpHeader> headers) {
    return request(HttpMethod::DELETE, url, std::move(headers), {});
}

void HttpClient::abort() {
    // Stub
}

} // namespace net
} // namespace hpactor
