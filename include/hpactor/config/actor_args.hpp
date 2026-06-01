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

#include <hpactor/types/types.hpp>

#include <string>
#include <unordered_map>

namespace hpactor::config {

/// \brief Concept for actors that accept TOML-derived configuration arguments.
///
/// Actors may optionally implement a static method:
///
/// \code{.cpp}
/// static result<void> configure_from_args(
///     const std::unordered_map<std::string, std::string>& args,
///     T& actor);
/// \endcode
///
/// The BootstrapEngine calls configure_from_args() after construction and
/// before on_activate() when the concrete type satisfies this concept.
///
/// \tparam T Actor type to check.
template <typename T>
concept ConfigurableActor =
    requires(T& actor, const std::unordered_map<std::string, std::string>& args) {
        { T::configure_from_args(args, actor) } -> std::same_as<result<void>>;
    };

} // namespace hpactor::config
