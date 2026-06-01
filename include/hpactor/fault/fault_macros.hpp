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

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/platform.hpp>

/// \def FAULT_INJECT(path)
/// \brief Deterministic fault injection hook placed at call sites.
///
/// Expands to an \c if statement whose body executes when a fault is scheduled
/// at \p path for the current (domain, tick). The enclosing code should place
/// the fault-action logic inside the controlled block:
///
/// \code{.cpp}
/// FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
///     return EnqueueResult::failure(EnqueueResultCode::kCapacityExceeded);
/// }
/// \endcode
///
/// \param path A dot-separated string literal naming the injection site. Must
///             match a registered \c FaultPoint path.
///
/// \note When \c HPACTOR_ENABLE_FAULT_INJECTION is \c OFF (compile-time),
///       the macro expands to \c if(false) and the compiler eliminates all
///       injection-site code. When enabled, the \c HPACTOR_UNLIKELY annotation
///       on the \c check() call ensures the cold branch is predicted not-taken.
///
/// \note Thread safety: reads the calling thread's installed
///       \c FaultController via \c FaultController::instance(). If no
///       controller is installed, the branch is never taken.
#if HPACTOR_ENABLE_FAULT_INJECTION
#    define FAULT_INJECT(path)                                                 \
        if (auto* _fc = ::hpactor::fault::FaultController::instance();         \
            _fc != nullptr && HPACTOR_UNLIKELY(_fc->check(path)))
#else
#    define FAULT_INJECT(path) if (false)
#endif
