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

#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/spawn.hpp>

namespace hpactor {

AsyncActor::AsyncActor()
    : mutex_(std::make_unique<std::mutex>()),
      cv_(std::make_unique<std::condition_variable>()) {}

AsyncActor::AsyncActor(EndPoint endpoint,
                       std::chrono::milliseconds timeout)
    : endpoint_(endpoint), timeout_(timeout),
      mutex_(std::make_unique<std::mutex>()),
      cv_(std::make_unique<std::condition_variable>()) {}

AsyncActor::AsyncActor(AsyncActor&& other) noexcept
    : endpoint_(other.endpoint_), timeout_(other.timeout_),
      mutex_(std::move(other.mutex_)), cv_(std::move(other.cv_)),
      ready_(other.ready_), cancelled_(other.cancelled_),
      response_(other.response_) {}

AsyncActor& AsyncActor::operator=(AsyncActor&& other) noexcept {
    if (this != &other) {
        endpoint_ = other.endpoint_;
        timeout_ = other.timeout_;
        mutex_ = std::move(other.mutex_);
        cv_ = std::move(other.cv_);
        ready_ = other.ready_;
        cancelled_ = other.cancelled_;
        response_ = other.response_;
    }
    return *this;
}

result<ActorRef> AsyncActor::get() {
    std::unique_lock<std::mutex> lock(*mutex_);
    if (cancelled_) {
        return result<ActorRef>::make(error(errors::unknown, "spawn "
                                                             "cancelled"));
    }

    bool timed_out = !cv_->wait_for(lock, timeout_, [this] { return ready_; });
    if (timed_out) {
        return result<ActorRef>::make(error(errors::timeout, "spawn request "
                                                             "timed out"));
    }

    if (response_.error_code != spawn_errors::success) {
        return result<ActorRef>::make(error(response_.error_code, "spawn "
                                                                  "failed"));
    }

    // Create ActorProxy for the remote actor using stack allocation
    ActorProxy proxy(response_.actor_addr, static_cast<net::Transport*>(nullptr));
    ActorRef ref(std::move(proxy));
    return result<ActorRef>::make(std::move(ref));
}

bool AsyncActor::ready() const {
    std::lock_guard<std::mutex> lock(*mutex_);
    return ready_ || cancelled_;
}

void AsyncActor::cancel() {
    std::lock_guard<std::mutex> lock(*mutex_);
    cancelled_ = true;
    ready_ = true;
    cv_->notify_all();
}

void AsyncActor::set_response(SpawnResponse response) {
    std::lock_guard<std::mutex> lock(*mutex_);
    response_ = response;
    ready_ = true;
    cv_->notify_all();
}

} // namespace hpactor