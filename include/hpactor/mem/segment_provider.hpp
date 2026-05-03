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

#include <hpactor/mem/size_class.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor::mem {

// Tier 0: Global segment provider. Acquires large mmap'd regions and carves
// them into slabs for thread-local caches. Thread-safe.
class SegmentProvider {
  public:
    struct SegmentInfo {
        void* base;
        size_t size;
    };

    struct Stats {
        size_t total_allocated{0};
        size_t active_segments{0};
    };

    static SegmentProvider& instance();

    // Acquire a slab of the given size class. Returns base pointer.
    void* acquire_slab(SizeClass sc);

    // Release a slab back. When all slabs in a segment are freed, munmap.
    void release_slab(void* slab, SizeClass sc);

    // Look up which segment a pointer belongs to.
    SegmentInfo lookup(void* ptr) const;

    // Size of a slab for a given size class.
    size_t slab_size(SizeClass sc) const;

    Stats stats() const;

  private:
    SegmentProvider() = default;

    static constexpr size_t kSegmentSize = 2 * 1024 * 1024; // 2MB
    static constexpr size_t kBaseSlabSize = 64 * 1024;      // 64KB default

    struct Segment {
        void* base{nullptr};
        size_t size{0};
        size_t offset{0};
        uint32_t ref_count{0}; // atomic not needed; protected by mutex_

        void inc_ref() { ++ref_count; }
        uint32_t dec_ref() { return --ref_count; }
    };

    void* carve_from_segment(SizeClass sc);
    void* allocate_new_segment(size_t size);

    mutable std::mutex mutex_;
    std::vector<Segment> segments_;
    std::unordered_map<void*, Segment*> addr_to_segment_;
};

} // namespace hpactor::mem
