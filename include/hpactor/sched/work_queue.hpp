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
#include <hpactor/adt/chaselev_deque.hpp>
#include <hpactor/adt/multi_priority_work_queue.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::sched {

struct WorkItem {
    ActorId actor;
    int64_t deadline_ns;
    uint64_t sequence;
    bool edf_scheduled = false; ///< True if this item was originally placed
                                ///< via the EDF path. Preserved across
                                ///< requeue cycles.
    /// Direct actor pointer populated by notify_ready_fast(). When non-null,
    /// execute_actor() uses it directly, eliminating the get_actor() hash
    /// lookup on every dispatch. Null means fall back to get_actor().
    /// Invariant: alive while queued — ActorReadyGate prevents post-termination
    /// scheduling; shutdown drains all items before destroying actors.
    EventBasedActor* actor_ptr{nullptr};
    /// Home worker index for cache-affine routing. Set to
    /// hash(actor_id) % num_workers at notify_ready time. UINT32_MAX means
    /// use round-robin. Preserved across requeue cycles.
    uint32_t home_worker{UINT32_MAX};
};

using adt::ChaselevDeque;
using MultiPriorityWorkQueue = adt::MultiPriorityWorkQueue<WorkItem>;

} // namespace hpactor::sched
