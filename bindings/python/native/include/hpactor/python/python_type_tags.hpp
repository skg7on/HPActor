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

#include <hpactor/msg/type_tag.hpp>

namespace hpactor::python {

/// \brief Python bridge wakeup signal. Sent from the Python-side notifier to
/// the
///        gateway when new commands are available for processing.
inline constexpr TypeTag kPythonWakeupTag = make_subsystem_tag(0xF0);

/// \brief Python actor command ingress. Carries a PythonCommand from the
///        interpreter thread to the native bridge actor.
inline constexpr TypeTag kPythonActorCommandTag = make_subsystem_tag(0xF1);

/// \brief Python actor failure notification. Carries a PythonCompletion with
///        failure details from the bridge actor to the runtime.
inline constexpr TypeTag kPythonActorFailedTag = make_subsystem_tag(0xF2);

/// \brief Python bridge inspect state. Carries an introspection request from
///        the CLI or metrics subsystem to the bridge runtime.
inline constexpr TypeTag kPythonInspectTag = make_subsystem_tag(0xF3);

} // namespace hpactor::python
