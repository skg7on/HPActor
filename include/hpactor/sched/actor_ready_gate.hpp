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

#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor {
class ActorSystem;
class EventBasedActor;
} // namespace hpactor

namespace hpactor::sched {

enum class ReadyAdmissionCode : uint8_t {
    Accepted,
    MissingActor,
    AlreadyReady,
    AlreadyRunning,
    Terminated,
    NotAdmissible,
};

struct ReadyAdmission {
    ReadyAdmissionCode code{ReadyAdmissionCode::NotAdmissible};

    bool accepted() const noexcept {
        return code == ReadyAdmissionCode::Accepted;
    }
};

class ActorReadyGate {
  public:
    explicit ActorReadyGate(ActorSystem& system) noexcept;

    ReadyAdmission try_mark_ready(ActorId actor) noexcept;
    ReadyAdmission mark_ready_already_admitted(EventBasedActor& actor) noexcept;

  private:
    ActorSystem& system_;
};

} // namespace hpactor::sched
