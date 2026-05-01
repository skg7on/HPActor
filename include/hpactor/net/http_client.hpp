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
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace net {

class EventLoop;
class HttpSerializer;
class HttpKeepAlivePool;

// ---------------------------------------------------------------------------
// HttpClient — HTTP egress for actor-to-external communication
// ---------------------------------------------------------------------------
class HttpClient {
  public:
    explicit HttpClient(EventLoop* loop);
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Core: async HTTP request with future response
    RpcFuture<StreamBuffer> request(HttpMethod method,
                                     const std::string& url,
                                     std::vector<HttpHeader> headers = {},
                                     StreamBuffer body = {});

    // Convenience methods
    RpcFuture<StreamBuffer> get(const std::string& url,
                                 std::vector<HttpHeader> headers = {});
    RpcFuture<StreamBuffer> post(const std::string& url,
                                  StreamBuffer body,
                                  std::vector<HttpHeader> headers = {});
    RpcFuture<StreamBuffer> put(const std::string& url,
                                 StreamBuffer body,
                                 std::vector<HttpHeader> headers = {});
    RpcFuture<StreamBuffer> del(const std::string& url,
                                 std::vector<HttpHeader> headers = {});

    // Cancel all in-flight requests
    void abort();

    // Configuration
    void set_default_timeout(std::chrono::milliseconds timeout) {
        default_timeout_ = timeout;
    }
    void set_max_retries(int retries) { max_retries_ = retries; }
    void set_keepalive(bool enable) { keepalive_enabled_ = enable; }
    void set_max_connections_per_host(size_t max) {
        max_conns_per_host_ = max;
    }

  private:
    // TODO: used when full HttpClient implementation is complete
    [[maybe_unused]] EventLoop* loop_ = nullptr;
    std::unordered_map<std::string, std::shared_ptr<HttpKeepAlivePool>> pools_;
    mutable std::mutex mutex_;
    std::chrono::milliseconds default_timeout_{5000};
    int max_retries_{3};
    bool keepalive_enabled_{true};
    size_t max_conns_per_host_{4};
};

} // namespace net
} // namespace hpactor
