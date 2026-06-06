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

#include <hpactor/msg/request_handle.hpp>
#include <hpactor/msg/request_timeout.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace hpactor {

class ActorSystem;

namespace sched {
class IScheduler;
} // namespace sched

/// \brief Tracks in-flight ask requests and correlates responses.
///
/// Owned by ActorSystem. When an actor calls context()->ask(), AskManager
/// generates a MessageId, stores a PendingAsk with the RequestHandle, and
/// schedules a timeout timer. When the target actor replies, AskManager
/// resolves the handle.
///
/// \note Thread safety: All public methods are internally synchronized.
///       Timeout callbacks fire on scheduler threads.
class AskManager {
  public:
    /// \brief Result of register_ask().
    struct RegistrationResult {
        /// \brief Handle the caller uses to wait for or poll the response.
        RequestHandle<StreamBuffer> handle;

        /// \brief MessageId assigned to this ask request.
        ///
        /// The caller sets this on the outgoing TypedMessage so the
        /// receiver can include it in the reply, enabling correlation.
        MessageId msg_id;
    };

    /// \brief Construct with scheduler reference for timeout timers.
    ///
    /// \param[in] scheduler Scheduler for timeout callbacks.
    /// \param[in] system ActorSystem for FailureEnvelope emission.
    explicit AskManager(sched::IScheduler* scheduler, ActorSystem* system);

    ~AskManager();

    AskManager(const AskManager&) = delete;
    AskManager& operator=(const AskManager&) = delete;

    /// \brief Register a new pending ask.
    ///
    /// Must be called BEFORE sending the request message.
    ///
    /// \param[in] requester_id ActorId of the asking actor.
    /// \param[in] target Target actor address (used for failure envelopes).
    /// \param[in] timeout Per-request timeout specification.
    /// \param[in] system_timeout_ms Fallback timeout from system config
    ///                              (used when timeout is default).
    /// \return A RegistrationResult with the handle and generated MessageId.
    [[nodiscard]] RegistrationResult
    register_ask(ActorId requester_id, ActorAddress target, RequestTimeout timeout,
                 std::chrono::milliseconds system_timeout_ms);

    /// \brief Called when a reply arrives for a tracked ask.
    ///
    /// \param[in] ask_msg_id The ask_message_id from the reply.
    /// \param[in] response The response payload.
    /// \return true if the ask was found and resolved, false if the ask
    ///         was not found (already timed out or never registered).
    bool on_response(uint64_t ask_msg_id, StreamBuffer response);

    /// \brief Called by the scheduler when a timeout fires.
    ///
    /// \param[in] ask_msg_id The message ID that timed out.
    void on_timeout(uint64_t ask_msg_id);

    /// \brief Cancel all pending asks (during shutdown).
    ///
    /// Resolves every pending handle with errors::unknown so that no
    /// caller is left blocked. Safe to call multiple times.
    void abort();

    /// \brief Number of currently pending asks.
    ///
    /// \return Count of in-flight ask requests.
    size_t pending_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

  private:
    /// \brief Internal record for a single in-flight ask.
    struct PendingAsk {
        /// \brief MessageId key in the pending_ map.
        uint64_t msg_id = 0;

        /// \brief ActorId of the actor that initiated the ask.
        ActorId requester_id{};

        /// \brief Target actor address.
        ActorAddress target{};

        /// \brief Handle that shares state with the caller's handle.
        ///
        /// The stored handle and the handle returned to the caller
        /// reference the same shared State, so resolving here unblocks
        /// the caller's get().
        RequestHandle<StreamBuffer> handle;
    };

    sched::IScheduler* scheduler_;
    ActorSystem* system_;
    std::unordered_map<uint64_t, std::unique_ptr<PendingAsk>> pending_;
    mutable std::mutex mutex_;
};

} // namespace hpactor
