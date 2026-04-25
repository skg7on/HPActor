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

#include <hpactor/net/frame.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/sched/scheduler.hpp>

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
template class RpcFuture<bytes>;

// -----------------------------------------------------------------------------
// RpcChannel implementation
// -----------------------------------------------------------------------------
RpcChannel::RpcChannel(net::Transport* transport, sched::IScheduler* scheduler)
    : transport_(transport), scheduler_(scheduler) {}

void RpcChannel::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, call] : pending_) {
        if (!call->ready_.load(std::memory_order_acquire)) {
            call->promise.set_value(
                result<bytes>::make(error(errors::unknown, "RPC channel "
                                                           "aborted")));
            call->ready_.store(true, std::memory_order_release);
        }
    }
    pending_.clear();
}

void RpcChannel::on_response(MessageId msg_id, const bytes& encoded_response) {
    std::unique_ptr<PendingCall> call;
    uint64_t key = msg_id.value();
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
    call->promise.set_value(result<bytes>::make(bytes(encoded_response)));
}

void RpcChannel::on_timeout(MessageId msg_id) {
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

    if (call_ptr->retry_count < call_ptr->max_retries) {
        call_ptr->retry_count++;
        schedule_retry(call_ptr);
    } else {
        call_ptr->ready_.store(true, std::memory_order_release);
        call_ptr->promise.set_value(
            result<bytes>::make(error(errors::timeout, "RPC call timed out")));
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(key);
    }
}

void RpcChannel::schedule_retry(PendingCall* call) {
    int64_t delay_ns = call->timeout.count() * 1000000;
    scheduler_->schedule_after(
        [this, msg_id = call->msg_id]() { on_timeout(msg_id); }, delay_ns);
    send_request(*call, true);
}

void RpcChannel::send_request(PendingCall& call, bool is_retry) {
    net::WireFrame frame;
    frame.sender = ActorAddress{};
    frame.receiver = call.target;
    frame.payload = call.encoded_request;
    frame.message_id = call.msg_id.value();
    frame.flags = net::WireFrame::RpcRequest;
    if (is_retry) {
        frame.flags |= net::WireFrame::RpcIdempotent;
    }

    bytes encoded = frame.encode();
    transport_->send(call.target, encoded);
}

RpcFuture<bytes>
RpcChannel::call_raw(const ActorAddress& target, const bytes& encoded_request,
                     std::chrono::milliseconds timeout_ms) {
    MessageId msg_id = MessageId::generate();

    auto promise_ptr = std::make_shared<std::promise<result<bytes>>>();
    auto future = promise_ptr->get_future();

    auto* call_ptr =
        new PendingCall{.msg_id = msg_id,
                        .target = target,
                        .encoded_request = encoded_request,
                        .timeout = timeout_ms,
                        .retry_count = 0,
                        .max_retries = 5,
                        .promise = std::move(*promise_ptr),
                        .enqueued_at = std::chrono::steady_clock::now(),
                        .ready_ = false};

    uint64_t key = msg_id.value();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.emplace(key, std::unique_ptr<PendingCall>(call_ptr));
    }

    send_request(*call_ptr, false);

    int64_t delay_ns = timeout_ms.count() * 1000000;
    scheduler_->schedule_after([this, msg_id]() { on_timeout(msg_id); }, delay_ns);

    return RpcFuture<bytes>(std::move(future), timeout_ms);
}

} // namespace hpactor
