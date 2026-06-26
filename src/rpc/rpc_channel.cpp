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

#include <hpactor/msg/frame.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <hpactor/fault/fault_macros.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// RpcFuture implementation
// -----------------------------------------------------------------------------
template <typename T>
RpcFuture<T>::RpcFuture(std::future<result<T>> inner,
                        std::chrono::milliseconds timeout)
    : inner_(std::move(inner)), timeout_(timeout) {}

template <typename T> result<T> RpcFuture<T>::get() {
    if (!inner_.valid()) {
        return result<T>::make(error(errors::unknown, "future not valid"));
    }

    auto status = inner_.wait_for(timeout_);
    if (status == std::future_status::timeout) {
        return result<T>::make(error(errors::timeout, "RPC call timed out"));
    }

    return inner_.get();
}

// Explicit instantiations
template class RpcFuture<StreamBuffer>;

// -----------------------------------------------------------------------------
// RpcChannel implementation
// -----------------------------------------------------------------------------
RpcChannel::RpcChannel(net::Transport* transport, sched::IScheduler* scheduler,
                       uint32_t default_max_retries)
    : transport_(transport), scheduler_(scheduler),
      default_max_retries_(default_max_retries) {}

void RpcChannel::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, call] : pending_) {
        if (!call->ready_.load(std::memory_order_acquire)) {
            call->promise.set_value(
                result<StreamBuffer>::make(error(errors::unknown, "RPC channel "
                                                                  "aborted")));
            call->ready_.store(true, std::memory_order_release);
        }
    }
    pending_.clear();
}

void RpcChannel::on_response(const RpcResponseFrame& response) {
    FAULT_INJECT("hpactor.rpc.response.delay") {
        _fc->stall(hpactor::fault::FaultDomain::kRpc, 3);
    }
    std::unique_ptr<PendingCall> call;
    uint64_t key = response.msg_id.value();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(key);
        if (it == pending_.end()) {
            return;
        }
        call = std::move(it->second);
        pending_.erase(it);
    }

    call->ready_.store(true, std::memory_order_release);
    call->promise.set_value(
        result<StreamBuffer>::make(StreamBuffer(response.payload)));
}

void RpcChannel::on_timeout(MessageId msg_id) {
    FAULT_INJECT("hpactor.rpc.timeout.drop") {
        return;
    }
    PendingCall* call_ptr = nullptr;
    uint64_t key = msg_id.value();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(key);
        if (it == pending_.end()) {
            return;
        }
        call_ptr = it->second.get();
    }

    // Enforce total deadline across retries
    if (call_ptr->deadline != std::chrono::steady_clock::time_point::max()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= call_ptr->deadline) {
            // Total deadline exceeded — fail permanently
            FAULT_INJECT("hpactor.rpc.deadline.drop") {
                return;
            }
            call_ptr->ready_.store(true, std::memory_order_release);
            call_ptr->promise.set_value(
                result<StreamBuffer>::make(error(errors::timeout, "RPC "
                                                                  "deadline "
                                                                  "expired")));
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.erase(key);
            return;
        }
    }

    if (call_ptr->retry_count < call_ptr->max_retries) {
        call_ptr->retry_count++;
        schedule_retry(call_ptr);
    } else {
        call_ptr->ready_.store(true, std::memory_order_release);
        call_ptr->promise.set_value(
            result<StreamBuffer>::make(error(errors::timeout, "RPC call timed "
                                                              "out")));
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(key);
    }
}

void RpcChannel::schedule_retry(PendingCall* call) {
    FAULT_INJECT("hpactor.rpc.retry.drop") {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(call->deadline - now);
    auto delay_ms = std::min(call->timeout, remaining);
    if (delay_ms.count() <= 0) {
        on_timeout(call->msg_id); // force immediate deadline expiry
        return;
    }
    int64_t delay_ns = delay_ms.count() * 1'000'000LL;
    scheduler_->schedule_after(
        [this, msg_id = call->msg_id]() { on_timeout(msg_id); }, delay_ns);
    send_request(*call, true);
}

void RpcChannel::send_request(PendingCall& call, bool is_retry) {
    FAULT_INJECT("hpactor.rpc.send.delay") {
        _fc->stall(hpactor::fault::FaultDomain::kRpc, 5);
    }
    net::WireFrame frame;
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(),
                  ActorAddress{});
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  call.target);
    frame.pb_envelope.mutable_data_frame()->set_payload(
        reinterpret_cast<const char*>(call.encoded_request.data()),
        call.encoded_request.size());
    frame.pb_envelope.mutable_data_frame()->set_message_id(call.msg_id.value());
    frame.pb_envelope.mutable_data_frame()->set_flags(net::WireFrame::RpcRequest);
    if (is_retry) {
        frame.pb_envelope.mutable_data_frame()->set_flags(
            frame.pb_envelope.data_frame().flags() | net::WireFrame::RpcIdempotent);
    }
    if (call.has_trace_context) {
        net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_trace_context(),
                      call.trace_context);
    }

    StreamBuffer encoded = frame.encode();
    transport_->send(call.target, encoded);
}

RpcFuture<StreamBuffer> RpcChannel::call_raw(const ActorAddress& target,
                                             const StreamBuffer& encoded_request,
                                             std::chrono::milliseconds timeout_ms) {
    return call_raw(target, encoded_request, timeout_ms, nullptr);
}

RpcFuture<StreamBuffer> RpcChannel::call_raw(const ActorAddress& target,
                                             const StreamBuffer& encoded_request,
                                             std::chrono::milliseconds timeout_ms,
                                             const TraceContext* parent_context) {
    MessageId msg_id = generate_message_id();

    auto promise_ptr = std::make_shared<std::promise<result<StreamBuffer>>>();
    auto future = promise_ptr->get_future();

    auto* call_ptr =
        new PendingCall{.msg_id = msg_id,
                        .target = target,
                        .encoded_request = encoded_request,
                        .timeout = timeout_ms,
                        .retry_count = 0,
                        .max_retries = static_cast<int>(default_max_retries_),
                        .promise = std::move(*promise_ptr),
                        .enqueued_at = std::chrono::steady_clock::now(),
                        .ready_ = false};
    if (parent_context != nullptr && parent_context->valid()) {
        call_ptr->has_trace_context = true;
        call_ptr->trace_context = *parent_context;
    }

    // Compute total deadline: timeout * (max_retries + 1)
    std::chrono::milliseconds total_budget = timeout_ms;
    if (call_ptr->max_retries > 0) {
        total_budget = timeout_ms * (call_ptr->max_retries + 1);
    }
    call_ptr->deadline = call_ptr->enqueued_at + total_budget;

    uint64_t key = msg_id.value();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.emplace(key, std::unique_ptr<PendingCall>(call_ptr));
    }

    send_request(*call_ptr, false);

    int64_t delay_ns = timeout_ms.count() * 1000000;
    scheduler_->schedule_after([this, msg_id]() { on_timeout(msg_id); }, delay_ns);

    return RpcFuture<StreamBuffer>(std::move(future), timeout_ms);
}

} // namespace hpactor
