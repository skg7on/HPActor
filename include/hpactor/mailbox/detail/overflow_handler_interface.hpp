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

#include <hpactor/mailbox/detail/overflow_context.hpp>
#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

namespace hpactor::mailbox::detail {

template <typename T> class IOverflowHandler {
  public:
    virtual ~IOverflowHandler() = default;

    virtual EnqueueResult
    handle(OverflowContext<T>& ctx, ReservationResult reason) = 0;
    virtual OverflowPolicy policy() const = 0;
};

} // namespace hpactor::mailbox::detail
