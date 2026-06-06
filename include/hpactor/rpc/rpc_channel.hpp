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

#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/rpc/rpc_types.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/tracing/span.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <unordered_map>

namespace hpactor {

using RpcResponseHandler = std::function<void(const RpcResponseFrame&)>;

// -----------------------------------------------------------------------------
// PendingCall - tracks in-flight RPC calls
// -----------------------------------------------------------------------------
struct PendingCall {
    MessageId msg_id;
    ActorAddress target;
    StreamBuffer encoded_request;
    std::chrono::milliseconds timeout;
    int retry_count = 0;
    int max_retries = 5;
    std::promise<result<StreamBuffer>> promise;
    std::chrono::steady_clock::time_point enqueued_at;
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};
    FailureSource source{FailureSource::Rpc};
    std::atomic<bool> ready_{false};
    bool has_trace_context{false};
    TraceContext trace_context{};
    tracing::SpanHandle client_span{};
};

// -----------------------------------------------------------------------------
// RpcFuture - wrapper around std::future with timeout
// -----------------------------------------------------------------------------
template <typename T> class RpcFuture {
  public:
    RpcFuture(std::future<result<T>> inner, std::chrono::milliseconds timeout);

    result<T> get(); // blocks until result available or timeout

    /// \brief Non-blocking readiness check.
    ///
    /// \return true if the future has been resolved (response arrived or
    /// error).
    bool ready() const {
        if (!inner_.valid())
            return true;
        return inner_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

  private:
    std::future<result<T>> inner_;
    std::chrono::milliseconds timeout_;
};

// -----------------------------------------------------------------------------
// RpcChannel - manages RPC calls with retry and timeout
// -----------------------------------------------------------------------------
class RpcChannel {
  public:
    explicit RpcChannel(net::Transport* transport, sched::IScheduler* scheduler);

    // Raw call - takes pre-encoded StreamBuffer, returns raw bytes response
    // Callers handle their own serialization/deserialization
    RpcFuture<StreamBuffer>
    call_raw(const ActorAddress& target, const StreamBuffer& encoded_request,
             std::chrono::milliseconds timeout_ms);

    // Cancel all pending calls
    void abort();

    // Trace-aware call_raw overload
    RpcFuture<StreamBuffer>
    call_raw(const ActorAddress& target, const StreamBuffer& encoded_request,
             std::chrono::milliseconds timeout_ms,
             const TraceContext* parent_context);

    // Handle response from transport layer
    void on_response(const RpcResponseFrame& response);

  private:
    void on_timeout(MessageId msg_id);
    void schedule_retry(PendingCall* call);
    void send_request(PendingCall& call, bool is_retry);

    net::Transport* transport_;
    sched::IScheduler* scheduler_;

    std::unordered_map<uint64_t, std::unique_ptr<PendingCall>> pending_;
    mutable std::mutex mutex_;
};

} // namespace hpactor
