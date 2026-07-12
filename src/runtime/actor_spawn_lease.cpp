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

#include <hpactor/runtime/actor_spawn_lease.hpp>

#include <hpactor/actor/system/actor_directory.hpp>

namespace hpactor {

ActorSpawnLease::ActorSpawnLease(ActorSpawnLease&& other) noexcept
    : actor_handle_(std::move(other.actor_handle_)),
      actor_id_(other.actor_id_),
      directory_(other.directory_),
      scheduler_(other.scheduler_),
      committed_(other.committed_) {
    other.committed_ = true; // prevent source destructor from rolling back
    other.directory_ = nullptr;
}

ActorSpawnLease& ActorSpawnLease::operator=(ActorSpawnLease&& other) noexcept {
    if (this != &other) {
        (void)rollback();
        actor_handle_ = std::move(other.actor_handle_);
        actor_id_ = other.actor_id_;
        directory_ = other.directory_;
        scheduler_ = other.scheduler_;
        committed_ = other.committed_;
        other.committed_ = true;
        other.directory_ = nullptr;
    }
    return *this;
}

ActorSpawnLease::~ActorSpawnLease() {
    (void)rollback();
}

const Actor& ActorSpawnLease::actor() const noexcept {
    return actor_handle_;
}

result<void> ActorSpawnLease::commit() noexcept {
    if (committed_ || empty())
        return result<void>::make();
    committed_ = true;
    return result<void>::make();
}

result<void> ActorSpawnLease::rollback() noexcept {
    if (committed_ || empty())
        return result<void>::make();

    // Reverse adoption: erase from directory.
    if (directory_ && actor_id_.value() != 0) {
        directory_->erase(actor_id_);
    }

    committed_ = true; // mark as rolled back (treated like committed)
    return result<void>::make();
}

bool ActorSpawnLease::committed() const noexcept {
    return committed_;
}

bool ActorSpawnLease::empty() const noexcept {
    return actor_id_.value() == 0;
}

ActorSpawnLease::ActorSpawnLease(Actor actor_handle, ActorId actor_id,
                                 ActorDirectory* directory,
                                 sched::IScheduler* scheduler) noexcept
    : actor_handle_(std::move(actor_handle)),
      actor_id_(actor_id),
      directory_(directory),
      scheduler_(scheduler) {}

} // namespace hpactor
