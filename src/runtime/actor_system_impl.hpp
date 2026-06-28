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

#include <hpactor/actor/actor_system.hpp>

#include <memory>

namespace hpactor {

/// \brief Private implementation of ActorSystem's runtime state.
///
/// Owns named state groups that will be extracted into cohesive runtime
/// components in later refactor phases.  This header is private to
/// hpactor_lib and must not be installed or included by public headers.
class ActorSystem::Impl final {
  public:
    Impl(ActorSystem& facade, const Config& config);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ActorSystem& facade;
    // State groups will be added by subsequent tasks.
};

} // namespace hpactor
