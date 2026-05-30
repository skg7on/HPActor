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

#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_telemetry.hpp>
#include <hpactor/mem/memory_tracker.hpp>
#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/fault/fault_macros.hpp>

#include <cstdlib>
#include <cstring>

namespace hpactor::mem {

thread_local ThreadLocalAllocator* t_tla = nullptr;
thread_local ActorId t_current_actor_id{};

namespace {

void* fallback_allocate(RegionType region, SizeClass sc, ActorId owner) noexcept {
    const size_t bytes = block_size(sc);
    void* raw = std::malloc(bytes); // NOLINT
    if (!raw) {
        return nullptr;
    }
    auto* header = AllocHeader::stamp(raw, sc, owner, region, true);
    CanaryFooter::stamp(header, bytes);
    return header->user_data();
}

void fallback_deallocate(AllocHeader* header) noexcept {
    std::free(header); // NOLINT
}

} // namespace

void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept {
    FAULT_INJECT("hpactor.allocator.oom") {
        return nullptr;
    }
    const SizeClass sc = class_for_size(user_bytes);
    const size_t charged_bytes = size_for_class(sc);
    auto& regions = MemoryRegionRegistry::instance();

    if (!regions.try_reserve(region, charged_bytes)) {
        return nullptr;
    }

    void* ptr = nullptr;
    if (t_tla) {
        ptr = t_tla->allocate(region, sc, owner);
    } else {
        ptr = fallback_allocate(region, sc, owner);
    }

    if (!ptr) {
        regions.cancel_reservation(region, charged_bytes);
        return nullptr;
    }

    regions.commit_alloc(region, charged_bytes);
    if constexpr (kMemoryTrackingEnabled) {
        MemoryTracker::instance().record_alloc(owner, charged_bytes);
    }
    MemoryTelemetry::instance().record_alloc(owner, region, sc, charged_bytes);
    return ptr;
}

void* allocate_class(SizeClass sc, ActorId owner) noexcept {
    return allocate(RegionType::kInternal, size_for_class(sc), owner);
}

void deallocate(void* user_ptr) noexcept {
    if (!user_ptr) {
        return;
    }

    auto* header = AllocHeader::from_user_data(user_ptr);
    const ActorId owner{header->owner_id};
    const RegionType region = header->region();
    const auto sc = static_cast<SizeClass>(header->size_class);
    const size_t charged_bytes = size_for_class(sc);
    const bool fallback = header->is_fallback();

    if constexpr (kMemoryTrackingEnabled) {
        MemoryTracker::instance().record_free(owner, charged_bytes);
    }
    MemoryRegionRegistry::instance().record_free(region, charged_bytes);
    MemoryTelemetry::instance().record_free(owner, region, sc, charged_bytes);

    if (fallback) {
        header->magic = kFreedMagic;
        fallback_deallocate(header);
        return;
    }

    if (t_tla) {
        t_tla->deallocate(user_ptr);
        return;
    }

    auto slab = SegmentProvider::instance().lookup_slab(header);
    if (slab.found && slab.owner_cache) {
        static_cast<SlabCache*>(slab.owner_cache)->deallocate(user_ptr);
    }
}

} // namespace hpactor::mem
