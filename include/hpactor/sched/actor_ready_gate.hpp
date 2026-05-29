// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

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
