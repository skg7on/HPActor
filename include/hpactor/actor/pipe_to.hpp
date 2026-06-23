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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <utility>

namespace hpactor {

/// \brief Pipe a resolved result to a target actor (Akka PipeTo pattern).
///
/// Routes a resolved \c result<T> to success/error callbacks keyed by
/// a target actor address. This is a combinatorial utility — no
/// ActorSystem or actor context is required.
///
/// For \c T=void, the success callback takes only \c (const ActorAddress&).
/// For non-void \c T, it takes \c (const ActorAddress&, T).
///
/// \tparam T The result type.
/// \tparam SuccessFn Callable \c void(const ActorAddress&, T) or
///         \c void(const ActorAddress&) for void result.
/// \tparam ErrorFn Callable \c void(const ActorAddress&, error).
/// \param[in,out] r The resolved result to pipe.
/// \param[in] target The actor address associated with this result.
/// \param[in] on_success Called with (target, [value]) if the result is ok.
/// \param[in] on_error Called with (target, error) if the result is an error.
///
/// Usage:
/// \code
///   auto r = handle.get();
///   pipe_to(r, receiver_addr,
///           [](const ActorAddress& t, MyResponse v) { ... },
///           [](const ActorAddress& t, error e) { ... });
/// \endcode
template <typename T, typename SuccessFn, typename ErrorFn>
void pipe_to(result<T>& r, const ActorAddress& target, SuccessFn&& on_success,
             ErrorFn&& on_error) {
    if (r.ok()) {
        if constexpr (std::is_void_v<T>) {
            on_success(target);
        } else {
            on_success(target, r.value());
        }
    } else {
        on_error(target, r.error());
    }
}

} // namespace hpactor
