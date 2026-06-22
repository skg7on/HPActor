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

#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {
namespace net {

class EventLoop;

/// \brief HTTP egress client for actor-to-external communication.
///
/// Sends async HTTP requests and returns an \c RpcFuture<StreamBuffer> for
/// the response body. Convenience methods wrap the common HTTP verbs.
///
/// \note This is a forward-looking stub — the event loop pointer is stored
///       but not yet wired to actual HTTP connection management.
class HttpClient {
  public:
    /// \brief Construct an HTTP client.
    ///
    /// \param[in] loop Event loop for async I/O.
    explicit HttpClient(EventLoop* loop);
    ~HttpClient();

    /// \name Non-copyable
    /// @{
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    /// @}

    /// \brief Send an async HTTP request.
    ///
    /// \param[in] method HTTP method.
    /// \param[in] url Target URL (scheme + host + path).
    /// \param[in] headers Optional request headers.
    /// \param[in] body Optional request body.
    /// \return Future that resolves to the response body.
    RpcFuture<StreamBuffer>
    request(HttpMethod method, const std::string& url,
            std::vector<HttpHeader> headers = {}, StreamBuffer body = {});

    /// \brief Send an HTTP GET request.
    ///
    /// \param[in] url Target URL.
    /// \param[in] headers Optional request headers.
    /// \return Future that resolves to the response body.
    RpcFuture<StreamBuffer>
    get(const std::string& url, std::vector<HttpHeader> headers = {});

    /// \brief Send an HTTP POST request.
    ///
    /// \param[in] url Target URL.
    /// \param[in] body Request body.
    /// \param[in] headers Optional request headers.
    /// \return Future that resolves to the response body.
    RpcFuture<StreamBuffer> post(const std::string& url, StreamBuffer body,
                                 std::vector<HttpHeader> headers = {});

    /// \brief Send an HTTP PUT request.
    ///
    /// \param[in] url Target URL.
    /// \param[in] body Request body.
    /// \param[in] headers Optional request headers.
    /// \return Future that resolves to the response body.
    RpcFuture<StreamBuffer> put(const std::string& url, StreamBuffer body,
                                std::vector<HttpHeader> headers = {});

    /// \brief Send an HTTP DELETE request.
    ///
    /// \param[in] url Target URL.
    /// \param[in] headers Optional request headers.
    /// \return Future that resolves to the response body.
    RpcFuture<StreamBuffer>
    del(const std::string& url, std::vector<HttpHeader> headers = {});

    /// \brief Cancel all in-flight requests.
    void abort();

    /// \brief Set the default request timeout.
    ///
    /// \param[in] timeout Timeout duration (default 5000 ms).
    void set_default_timeout(std::chrono::milliseconds timeout) {
        default_timeout_ = timeout;
    }

    /// \brief Set the maximum number of retries for failed requests.
    ///
    /// \param[in] retries Maximum retry count (default 3).
    void set_max_retries(int retries) {
        max_retries_ = retries;
    }

  private:
    [[maybe_unused]] EventLoop* loop_ = nullptr;
    std::chrono::milliseconds default_timeout_{5000};
    int max_retries_{3};
};

} // namespace net
} // namespace hpactor
