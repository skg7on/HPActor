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

#include <hpactor/msg/delivery_receipt.hpp>

namespace hpactor::msg {

DeliveryReceipt::DeliveryReceipt(mailbox::DeliveryResult result)
    : state_(std::make_shared<SharedState>()) {
    state_->msg_id = result.message_id;
    state_->result = std::move(result);
}

DeliveryReceipt::DeliveryReceipt(std::shared_ptr<SharedState> state)
    : state_(std::move(state)) {}

bool DeliveryReceipt::ready() const noexcept {
    if (!state_)
        return false;
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->result.has_value();
}

mailbox::DeliveryResult DeliveryReceipt::get() const {
    if (!state_)
        return {};
    std::unique_lock<std::mutex> lk(state_->mtx);
    state_->cv.wait(lk, [this] { return state_->result.has_value(); });
    return *state_->result;
}

std::optional<mailbox::DeliveryResult> DeliveryReceipt::try_get() const noexcept {
    if (!state_)
        return std::nullopt;
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (state_->result.has_value()) {
        return *state_->result;
    }
    return std::nullopt;
}

void DeliveryReceipt::on_complete(std::function<void(mailbox::DeliveryResult)> callback) {
    if (!state_)
        return;
    std::unique_lock<std::mutex> lk(state_->mtx);
    // Reject when a fixed port is already installed.
    if (state_->fixed_port_set) {
        return;
    }
    if (state_->result.has_value()) {
        auto result = *state_->result;
        lk.unlock();
        if (callback)
            callback(result);
    } else {
        state_->callback = std::move(callback);
    }
}

bool DeliveryReceipt::on_complete(CompletionPort<mailbox::DeliveryResult> port) {
    if (!state_)
        return false;
    std::unique_lock<std::mutex> lk(state_->mtx);
    // Reject when either form of callback is already installed.
    if (state_->fixed_port_set || state_->callback) {
        return false;
    }
    if (state_->result.has_value()) {
        auto result = *state_->result;
        lk.unlock();
        if (port)
            port(result);
        return true;
    }
    state_->fixed_port = std::move(port);
    state_->fixed_port_set = true;
    return true;
}

void DeliveryReceipt::cancel() {
    if (!state_)
        return;
    CompletionPort<mailbox::DeliveryResult> port;
    std::function<void(mailbox::DeliveryResult)> cb;
    {
        std::unique_lock<std::mutex> lk(state_->mtx);
        if (state_->result.has_value()) {
            return;
        }
        mailbox::DeliveryResult cancelled;
        cancelled.status = mailbox::DeliveryStatus::Cancelled;
        cancelled.message_id = state_->msg_id;
        state_->result = cancelled;
        cb = std::move(state_->callback);
        port = std::move(state_->fixed_port);
        state_->fixed_port_set = false;
    }
    state_->cv.notify_all();
    if (port)
        port(*state_->result);
    if (cb)
        cb(*state_->result);
}

MessageId DeliveryReceipt::message_id() const noexcept {
    if (!state_)
        return {};
    std::lock_guard<std::mutex> lk(state_->mtx);
    return state_->msg_id;
}

void DeliveryReceipt::SharedState::resolve(mailbox::DeliveryResult r) {
    CompletionPort<mailbox::DeliveryResult> port;
    std::function<void(mailbox::DeliveryResult)> cb;
    {
        std::unique_lock<std::mutex> lk(mtx);
        if (result.has_value())
            return;
        msg_id = r.message_id;
        result = std::move(r);
        cb = std::move(callback);
        port = std::move(fixed_port);
        fixed_port_set = false;
    }
    cv.notify_all();
    if (port)
        port(*result);
    if (cb)
        cb(*result);
}

} // namespace hpactor::msg
