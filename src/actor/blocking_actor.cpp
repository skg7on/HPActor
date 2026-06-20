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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/blocking_actor.hpp>

namespace hpactor {

BlockingActor::BlockingActor(ActorContext* ctx, ActorSystem& sys)
    : LocalActor(ctx, sys) {}

BlockingActor::BlockingActor(ActorId id, ActorContext* ctx, ActorSystem& sys)
    : LocalActor(id, ctx, sys) {}

BlockingActor::~BlockingActor() {
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void BlockingActor::receive(TypedMessage& /*msg*/) {
    // Default no-op. Subclasses override this to handle messages.
}

void BlockingActor::on_activate() {
    LocalActor::on_activate();

    // Wire continuation callback on the mailbox so that when a message
    // transitions the mailbox from empty to non-empty, the condition
    // variable is signalled and the blocking thread wakes up.
    if (mailbox_) {
        mailbox_->set_continuation_callback([this]() {
            {
                std::lock_guard<std::mutex> lock(cv_mutex_);
                message_arrived_.store(true, std::memory_order_release);
            }
            cv_.notify_one();
        });
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&BlockingActor::thread_loop, this);
}

void BlockingActor::on_deactivate() {
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    LocalActor::on_deactivate();
}

void BlockingActor::thread_loop() {
    actor_state_.set(ActorState::kRunning);

    while (running_.load(std::memory_order_acquire)) {
        TypedMessage msg;

        {
            std::unique_lock<std::mutex> lock(cv_mutex_);

            // Try non-blocking pop first
            if (mailbox_ && mailbox_->try_pop(msg)) {
                message_arrived_.store(false, std::memory_order_release);
            } else {
                // Block until a message arrives or we're shutting down
                actor_state_.set(ActorState::kIdle);
                cv_.wait(lock, [this]() {
                    return message_arrived_.load(std::memory_order_acquire) ||
                           !running_.load(std::memory_order_acquire);
                });
                if (!running_.load(std::memory_order_acquire))
                    break;

                message_arrived_.store(false, std::memory_order_release);
                actor_state_.set(ActorState::kRunning);

                // Retry pop after wakeup
                if (mailbox_)
                    mailbox_->try_pop(msg);
            }
        }

        // Dispatch the message via the virtual receive()
        if (msg.type_id() != TypeTag::Invalid) {
            receive(msg);
        }
    }

    actor_state_.set(ActorState::kTerminated);
}

void BlockingActor::await_all_other_actors_done() {
    // Stub: in a full implementation this would poll ActorSystem for
    // active actor count and block until only this actor remains.
}

} // namespace hpactor
