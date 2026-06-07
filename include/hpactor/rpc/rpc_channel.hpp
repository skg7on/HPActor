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

/// \brief Callback invoked when an RPC response frame arrives.
///
/// The handler receives the full \c RpcResponseFrame containing the
/// correlation \c MessageId and response payload.
using RpcResponseHandler = std::function<void(const RpcResponseFrame&)>;

// -----------------------------------------------------------------------------
// PendingCall - tracks in-flight RPC calls
// -----------------------------------------------------------------------------

/// \brief Tracks the state of a single in-flight RPC call.
///
/// Owns the request payload, retry bookkeeping, the response promise,
/// and the total deadline. Fields are public so that \c RpcChannel
/// internals can read and mutate them under the channel mutex.
struct PendingCall {
    /// \brief Correlation id for this RPC call.
    MessageId msg_id;

    /// \brief Target actor address for the request.
    ActorAddress target;

    /// \brief Serialized request payload (owned by this call).
    StreamBuffer encoded_request;

    /// \brief Per-retry timeout.
    std::chrono::milliseconds timeout;

    /// \brief Number of retries already attempted.
    int retry_count = 0;

    /// \brief Maximum number of retries before giving up.
    int max_retries = 5;

    /// \brief Promise resolved when the response arrives or the call
    ///        fails permanently.
    std::promise<result<StreamBuffer>> promise;

    /// \brief Wall-clock time when the call was first enqueued.
    std::chrono::steady_clock::time_point enqueued_at;

    /// \brief Absolute deadline for all retries combined.
    ///
    /// Computed as \c enqueued_at + timeout * (max_retries + 1).
    /// Defaults to \c time_point::max() (no deadline) until set by
    /// \c call_raw().
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};

    /// \brief Failure subsystem origin for metric/diagnostic attribution.
    FailureSource source{FailureSource::Rpc};

    /// \brief Atomic flag set to \c true when the call is resolved
    ///        (response or permanent failure).
    ///
    /// \note Used for lock-free readiness checks outside the mutex.
    std::atomic<bool> ready_{false};

    /// \brief Whether \c trace_context contains valid trace data.
    bool has_trace_context{false};

    /// \brief Parent trace context for distributed tracing propagation.
    TraceContext trace_context{};

    /// \brief Client-side span handle for tracing RPC latency.
    tracing::SpanHandle client_span{};
};

// -----------------------------------------------------------------------------
// RpcFuture - wrapper around std::future with timeout
// -----------------------------------------------------------------------------

/// \brief Move-only future wrapper that enforces a per-call timeout on
///        \c get().
///
/// Wraps a \c std::future<result<T>> and provides a blocking \c get()
/// that returns \c error(errors::timeout) if the result is not ready
/// within the timeout specified at construction.
///
/// \tparam T The response payload type (typically \c StreamBuffer).
template <typename T> class RpcFuture {
  public:
    /// \brief Constructs the future from an inner \c std::future and
    ///        a timeout.
    ///
    /// \param[in] inner The underlying future that will hold the result.
    /// \param[in] timeout Maximum time \c get() will block.
    /// \note Ownership: takes ownership of \p inner via move.
    RpcFuture(std::future<result<T>> inner, std::chrono::milliseconds timeout);

    /// \brief Block until the result is available or the timeout expires.
    ///
    /// \return \c result<T> containing the response payload on success,
    ///         or an error:
    ///         - \c errors::timeout if the per-call timeout expires.
    ///         - \c errors::unknown if the inner future is not valid.
    /// \note Blocks the calling thread for up to \c timeout_.
    ///       Callable from non-actor threads.
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
    /// \brief Underlying future holding the response or error.
    std::future<result<T>> inner_;

    /// \brief Maximum blocking duration for \c get().
    std::chrono::milliseconds timeout_;
};

// -----------------------------------------------------------------------------
// RpcChannel - manages RPC calls with retry and timeout
// -----------------------------------------------------------------------------

/// \brief Client-side RPC channel that manages at-least-once request/response
///        calls with automatic retry and deadline enforcement.
///
/// Owns the map of in-flight \c PendingCall objects, protected by a mutex.
/// Each call is assigned a unique \c MessageId, a total deadline computed as
/// \c timeout * (max_retries + 1), and a per-retry timeout timer via the
/// scheduler. On response, the channel correlates by \c MessageId and
/// resolves the corresponding promise. On timeout, the channel retries up
/// to \c max_retries before failing permanently.
///
/// \note Thread safety: externally synchronized via \c mutex_. Safe to
///       call from non-actor threads (e.g., \c ActorProxy send paths).
///       The scheduler callbacks for timeout fire on the scheduler's
///       timer thread and also acquire the mutex.
class RpcChannel {
  public:
    /// \brief Constructs an RPC channel.
    ///
    /// \param[in] transport The network transport used to send requests.
    ///                      Must outlive this channel.
    /// \param[in] scheduler The scheduler used to schedule retry timers.
    ///                      Must outlive this channel.
    /// \param[in] default_max_retries Maximum retries per call (default 3).
    ///                                Overridable per-call via
    ///                                \c PendingCall::max_retries.
    explicit RpcChannel(net::Transport* transport, sched::IScheduler* scheduler,
                        uint32_t default_max_retries = 3);

    /// \brief Send an RPC request and return a future for the response.
    ///
    /// Generates a unique \c MessageId, enqueues a \c PendingCall with
    /// a total deadline of \c timeout_ms * (max_retries + 1), sends the
    /// request via the transport, and schedules the first timeout timer.
    ///
    /// \param[in] target The destination actor address.
    /// \param[in] encoded_request Pre-serialized request payload. Copied
    ///                            into the \c PendingCall.
    /// \param[in] timeout_ms Per-retry timeout. The total deadline is
    ///                       \c timeout_ms * (max_retries + 1).
    /// \return \c RpcFuture<StreamBuffer> that resolves with the response
    ///         payload or an error (\c errors::timeout,
    ///         \c errors::unknown on abort).
    // Raw call - takes pre-encoded StreamBuffer, returns raw bytes response
    // Callers handle their own serialization/deserialization
    RpcFuture<StreamBuffer>
    call_raw(const ActorAddress& target, const StreamBuffer& encoded_request,
             std::chrono::milliseconds timeout_ms);

    /// \brief Cancel all pending RPC calls.
    ///
    /// Resolves every in-flight call with \c error(errors::unknown, "RPC
    /// channel aborted") and clears the pending map.
    ///
    /// \post All previously returned futures are ready.
    /// \note Thread safety: acquires \c mutex_.
    // Cancel all pending calls
    void abort();

    /// \brief Send an RPC request with parent trace context for
    ///        distributed tracing propagation.
    ///
    /// \param[in] target The destination actor address.
    /// \param[in] encoded_request Pre-serialized request payload.
    /// \param[in] timeout_ms Per-retry timeout.
    /// \param[in] parent_context Parent trace context for span
    ///                           propagation. If \c nullptr or invalid,
    ///                           tracing is skipped for this call.
    /// \return \c RpcFuture<StreamBuffer> that resolves with the response
    ///         payload or an error.
    // Trace-aware call_raw overload
    RpcFuture<StreamBuffer>
    call_raw(const ActorAddress& target, const StreamBuffer& encoded_request,
             std::chrono::milliseconds timeout_ms,
             const TraceContext* parent_context);

    /// \brief Handle an incoming RPC response from the transport layer.
    ///
    /// Looks up the pending call by \c response.msg_id, removes it from
    /// the pending map, and resolves its promise with the response
    /// payload.
    ///
    /// \param[in] response The response frame received from the transport.
    /// \note Thread safety: acquires \c mutex_. If the \c msg_id is not
    ///       found in the pending map, the response is silently dropped
    ///       (already resolved or spurious).
    // Handle response from transport layer
    void on_response(const RpcResponseFrame& response);

  private:
    /// \brief Handle a per-retry timeout for a specific call.
    ///
    /// Checks the total deadline first. If the deadline has expired,
    /// fails the call permanently. Otherwise retries up to
    /// \c max_retries, re-sending the request with the
    /// \c RpcIdempotent flag set.
    ///
    /// \param[in] msg_id The message id of the timed-out call.
    void on_timeout(MessageId msg_id);

    /// \brief Schedule a retry via the scheduler and re-send the request.
    ///
    /// \param[in] call Pointer to the pending call. Must not be
    ///                 \c nullptr. The caller must hold \c mutex_
    ///                 or otherwise ensure the pointer stays alive.
    void schedule_retry(PendingCall* call);

    /// \brief Encode and send a request frame over the transport.
    ///
    /// Constructs a \c WireFrame with the \c RpcRequest flag. On retry,
    /// also sets the \c RpcIdempotent flag. Propagates the trace context
    /// if present.
    ///
    /// \param[in,out] call The pending call whose payload is sent.
    /// \param[in] is_retry \c true if this is a retransmission (adds
    ///                     \c RpcIdempotent flag).
    void send_request(PendingCall& call, bool is_retry);

    /// \brief Network transport for sending requests.
    ///
    /// Non-owning pointer. Must outlive this channel.
    net::Transport* transport_;

    /// \brief Scheduler for retry/timeout timer callbacks.
    ///
    /// Non-owning pointer. Must outlive this channel.
    sched::IScheduler* scheduler_;

    /// \brief Default max retries applied to new calls.
    uint32_t default_max_retries_ = 3;

    /// \brief Map of in-flight calls keyed by \c MessageId::value().
    ///
    /// Protected by \c mutex_. Values are \c unique_ptr for stable
    /// addresses across retry/timeout callbacks.
    std::unordered_map<uint64_t, std::unique_ptr<PendingCall>> pending_;

    /// \brief Mutex protecting \c pending_ and individual call state.
    mutable std::mutex mutex_;
};

} // namespace hpactor
