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

#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/failure_envelope.hpp>
#include <hpactor/sched/scheduler_interfaces.hpp>

namespace hpactor {

AskManager::AskManager(sched::IScheduler* scheduler, ActorSystem* system)
    : scheduler_(scheduler), system_(system) {}

AskManager::~AskManager() {
    abort();
}

AskManager::RegistrationResult
AskManager::register_ask(ActorId requester_id, ActorAddress target,
                         RequestTimeout timeout,
                         std::chrono::milliseconds system_timeout_ms) {
    uint64_t key = generate_message_id().value();

    // Compute the absolute deadline for the handle and the delay for the
    // timeout timer.
    auto now = std::chrono::steady_clock::now();
    auto deadline =
        timeout.is_default() ? now + system_timeout_ms : timeout.deadline();
    auto effective_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (effective_ms.count() < 0) {
        effective_ms = std::chrono::milliseconds(0);
    }

    // Create shared state and two handles that both reference it.
    // handle_for_caller is returned to the caller; handle_for_pending is
    // stored in PendingAsk so that on_response / on_timeout can resolve it.
    auto state = std::make_shared<RequestHandle<StreamBuffer>::State>();
    state->deadline = deadline;
    state->msg_id = MessageId(key);

    auto pending = std::make_unique<PendingAsk>();
    pending->msg_id = key;
    pending->requester_id = requester_id;
    pending->target = target;
    pending->handle = RequestHandle<StreamBuffer>(state);
    auto reg_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now.time_since_epoch())
                      .count();
    pending->registered_at_ns = static_cast<uint64_t>(reg_ns);

    RegistrationResult result;
    result.msg_id = MessageId(key);
    result.handle = RequestHandle<StreamBuffer>(state);

    // Insert into the map before scheduling the timer so the timeout
    // callback can find the entry.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.emplace(key, std::move(pending));
    }

    // Schedule the timeout callback. If on_response() already resolved the
    // ask between the emplace above and this line, the callback will find
    // nothing and return harmlessly.
    uint64_t key_copy = key;
    int64_t delay_ns = effective_ms.count() * 1'000'000LL;
    scheduler_->schedule_after([this, key_copy]() { on_timeout(key_copy); },
                               delay_ns);

    return result;
}

bool AskManager::on_response(uint64_t ask_msg_id, StreamBuffer response) {
    std::unique_ptr<PendingAsk> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(ask_msg_id);
        if (it == pending_.end()) {
            return false;
        }
        pending = std::move(it->second);
        pending_.erase(it);
    }

    // Resolve outside the lock to avoid re-entrancy issues if the
    // resolution notifies a waiting thread that re-enters AskManager.
    pending->handle.resolve(result<StreamBuffer>::make(std::move(response)));
    return true;
}

void AskManager::on_timeout(uint64_t ask_msg_id) {
    std::unique_ptr<PendingAsk> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(ask_msg_id);
        if (it == pending_.end()) {
            // Already resolved by on_response() or aborted.
            return;
        }
        pending = std::move(it->second);
        pending_.erase(it);
    }

    // Emit a failure envelope for observability before resolving.
    if (system_) {
        ActorAddress requester_addr(EndPoint{Ipv4Endpoint{0x7F000001, 0}},
                                    ActorType{0}, pending->requester_id, 0);
        FailureEnvelope env = make_failure_envelope(
            FailureReason::Timeout, pending->requester_id, requester_addr,
            pending->target, MessageId{pending->msg_id}, TraceContext{},
            FailureSource::ActorRuntime, "ask timed out");
        // Failure envelope is stack-allocated; metrics emission will be
        // wired in a follow-up task when AskManager is integrated with
        // the ActorSystem metrics ring buffer.
        (void)env;
    }

    pending->handle.resolve_error(error(errors::timeout, "local ask timed "
                                                         "out"));
}

std::vector<AskManager::SnapshotEntry> AskManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SnapshotEntry> result;
    result.reserve(pending_.size());

    auto now = std::chrono::steady_clock::now();
    auto now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());

    for (auto& [id, pending] : pending_) {
        SnapshotEntry entry;
        entry.msg_id = pending->msg_id;
        entry.requester_id = pending->requester_id.value();
        uint64_t diff_ns = (now_ns > pending->registered_at_ns)
                               ? (now_ns - pending->registered_at_ns)
                               : 0;
        entry.elapsed_ms = diff_ns / 1'000'000ULL;
        entry.deadline_remaining_ms = 0;
        result.push_back(entry);
    }
    return result;
}

void AskManager::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, pending] : pending_) {
        pending->handle.resolve_error(error(errors::unknown, "ask manager "
                                                             "aborted"));
    }
    pending_.clear();
}

} // namespace hpactor
