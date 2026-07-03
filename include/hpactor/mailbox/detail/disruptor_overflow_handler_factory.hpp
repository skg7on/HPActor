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

#include <hpactor/mailbox/detail/disruptor_overflow_handler_interface.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_block_when_allowed_handler.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_dead_letter_handler.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_drop_lowest_priority_handler.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_drop_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_drop_oldest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_reject_newest_handler.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_signal_only_handler.hpp>
#include <hpactor/mailbox/detail/handlers/disruptor_spill_to_overflow_handler.hpp>
#include <hpactor/msg/enqueue_result.hpp>

#include <memory>

namespace hpactor::mailbox::detail {

/// \brief Factory function that maps an \c OverflowPolicy to a concrete
///        \c IDisruptorOverflowHandler instance.
///
/// The returned handler is a heap-allocated unique_ptr owned by the caller
/// (typically \c DisruptorActorMailboxCore). \c BlockWhenAllowed falls back
/// to \c DisruptorRejectNewestHandler by default.
///
/// \tparam EnvelopeType The disruptor message envelope type.
/// \param[in] policy The overflow policy to instantiate.
/// \return A unique_ptr to the corresponding handler. Never returns
///         \c nullptr.
/// \note Thread safety: safe to call from any thread. Returns a new
///       heap-allocated instance with no shared state.
template <typename EnvelopeType>
[[nodiscard]] std::unique_ptr<IDisruptorOverflowHandler<EnvelopeType>>
make_disruptor_overflow_handler(OverflowPolicy policy) {
    switch (policy) {
        case OverflowPolicy::RejectNewest:
            return std::make_unique<DisruptorRejectNewestHandler<EnvelopeType>>();
        case OverflowPolicy::DropNewest:
            return std::make_unique<DisruptorDropNewestHandler<EnvelopeType>>();
        case OverflowPolicy::DropOldest:
            return std::make_unique<DisruptorDropOldestHandler<EnvelopeType>>();
        case OverflowPolicy::DeadLetter:
            return std::make_unique<DisruptorDeadLetterHandler<EnvelopeType>>();
        case OverflowPolicy::SignalOnly:
            return std::make_unique<DisruptorSignalOnlyHandler<EnvelopeType>>();
        case OverflowPolicy::SpillToOverflowQueue:
            return std::make_unique<DisruptorSpillToOverflowHandler<EnvelopeType>>();
        case OverflowPolicy::DropLowestPriority:
            return std::make_unique<DisruptorDropLowestPriorityHandler<EnvelopeType>>();
        case OverflowPolicy::BlockWhenAllowed:
            return std::make_unique<DisruptorBlockWhenAllowedHandler<EnvelopeType>>();
        default:
            return std::make_unique<DisruptorRejectNewestHandler<EnvelopeType>>();
    }
}

} // namespace hpactor::mailbox::detail
