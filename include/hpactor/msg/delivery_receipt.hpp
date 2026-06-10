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

#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace hpactor::msg {

class OutboundDeliveryTracker;

/// \brief Move-only handle for the eventual outcome of a tracked delivery.
///
/// Returned by \c try_send() when \c DeliveryMode is \c AtLeastOnce or
/// \c DurableAtLeastOnce. For \c BestEffort and \c ObservableBestEffort,
/// wraps an already-resolved \c DeliveryResult.
///
/// Callers can poll (\c try_get()), block (\c get()), register a callback
/// (\c on_complete()), or discard the receipt — the runtime still retries
/// until exhaustion regardless of whether the receipt is observed.
class DeliveryReceipt {
  public:
    /// \brief Shared state for pending delivery resolution.
    ///
    /// Created by \c OutboundDeliveryTracker and shared between the tracker
    /// and all copies of the receipt handle. Protected by a mutex.
    struct SharedState {
        std::mutex mtx;
        std::condition_variable cv;
        std::optional<mailbox::DeliveryResult> result;
        std::function<void(mailbox::DeliveryResult)> callback;
        MessageId msg_id{};

        /// \brief Resolve the state with a final result.
        ///
        /// No-op if already resolved. Notifies waiters and invokes the
        /// callback if set.
        ///
        /// \param[in] r The final delivery result.
        void resolve(mailbox::DeliveryResult r);
    };

    /// \brief Default-constructed receipt is invalid (no shared state).
    DeliveryReceipt() = default;
    ~DeliveryReceipt() = default;

    /// \brief Construct from an already-resolved \c DeliveryResult.
    ///
    /// Used for \c BestEffort and \c ObservableBestEffort modes where
    /// the outcome is known immediately.
    ///
    /// \param[in] result The delivery result to wrap.
    explicit DeliveryReceipt(mailbox::DeliveryResult result);

    // Move-only
    DeliveryReceipt(DeliveryReceipt&&) noexcept = default;
    DeliveryReceipt& operator=(DeliveryReceipt&&) noexcept = default;

    /// \brief Returns true when the final result is available (non-blocking).
    ///
    /// \retval true  The result is available via \c get() or \c try_get().
    /// \retval false The delivery is still pending and no result has arrived.
    [[nodiscard]] bool ready() const noexcept;

    /// \brief Block until the result is available.
    ///
    /// \warning Only call from a blocking-actor thread or non-actor thread.
    ///          Calling from an event-based actor will block the scheduler.
    /// \return The final delivery result.
    [[nodiscard]] mailbox::DeliveryResult get() const;

    /// \brief Non-blocking attempt to retrieve the result.
    ///
    /// \return The result if available, or \c std::nullopt if still pending.
    [[nodiscard]] std::optional<mailbox::DeliveryResult> try_get() const noexcept;

    /// \brief Register a callback to be invoked when the result arrives.
    ///
    /// If the receipt is already resolved, the callback fires synchronously
    /// on the calling thread. Otherwise, it fires on whichever thread calls
    /// \c SharedState::resolve().
    ///
    /// \param[in] callback The callback to invoke. Must not block.
    void on_complete(std::function<void(mailbox::DeliveryResult)> callback);

    /// \brief Cancel tracking. The runtime stops retrying.
    ///
    /// Resolves the receipt with \c DeliveryStatus::Cancelled if not already
    /// resolved. No-op if already resolved.
    void cancel();

    /// \brief The message id this receipt tracks.
    ///
    /// \return The \c MessageId, or a default-constructed \c MessageId
    ///         if the receipt is default-constructed (invalid).
    [[nodiscard]] MessageId message_id() const noexcept;

  private:
    friend class OutboundDeliveryTracker;

    /// \brief Construct from a pre-existing shared state.
    ///
    /// Used by \c OutboundDeliveryTracker to create receipts linked to
    /// its internal state.
    ///
    /// \param[in] state The shared state to link to.
    explicit DeliveryReceipt(std::shared_ptr<SharedState> state);

    std::shared_ptr<SharedState> state_;
};

} // namespace hpactor::msg
