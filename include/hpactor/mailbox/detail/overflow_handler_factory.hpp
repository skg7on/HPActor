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

#include <hpactor/mailbox/detail/handlers/dead_letter_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_lowest_priority_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/drop_oldest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/reject_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/signal_only_handler.hpp>
#include <hpactor/mailbox/detail/handlers/spill_to_overflow_handler.hpp>
#include <hpactor/mailbox/detail/overflow_handler_interface.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <memory>

namespace hpactor::mailbox::detail {

/// \brief Factory function that maps an \c OverflowPolicy to a concrete
///        \c IOverflowHandler instance.
///
/// The returned handler is a heap-allocated unique_ptr owned by the caller
/// (typically \c MPSCActorMailbox). \c BlockWhenAllowed falls back to
/// \c RejectNewestHandler by default.
///
/// \tparam T Message type stored in the mailbox.
/// \param[in] policy The overflow policy to instantiate.
/// \return A unique_ptr to the corresponding handler. Never returns
///         \c nullptr.
/// \note Thread safety: safe to call from any thread. Returns a new
///       heap-allocated instance with no shared state.
template <typename T>
[[nodiscard]] std::unique_ptr<IOverflowHandler<T>>
make_overflow_handler(OverflowPolicy policy) {
    switch (policy) {
        case OverflowPolicy::RejectNewest:
            return std::make_unique<RejectNewestHandler<T>>();
        case OverflowPolicy::DropNewest:
            return std::make_unique<DropNewestHandler<T>>();
        case OverflowPolicy::DropOldest:
            return std::make_unique<DropOldestHandler<T>>();
        case OverflowPolicy::DeadLetter:
            return std::make_unique<DeadLetterHandler<T>>();
        case OverflowPolicy::SignalOnly:
            return std::make_unique<SignalOnlyHandler<T>>();
        case OverflowPolicy::SpillToOverflowQueue:
            return std::make_unique<SpillToOverflowHandler<T>>();
        case OverflowPolicy::DropLowestPriority:
            return std::make_unique<DropLowestPriorityHandler<T>>();
        case OverflowPolicy::BlockWhenAllowed:
        default:
            return std::make_unique<RejectNewestHandler<T>>();
    }
}

} // namespace hpactor::mailbox::detail
