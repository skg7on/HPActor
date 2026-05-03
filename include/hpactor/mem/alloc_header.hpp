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
#include <hpactor/types/types.hpp>

#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

inline constexpr uint32_t kAllocMagic = 0xAC70AC70;
inline constexpr uint32_t kFreedMagic = 0xDEADDEAD;

// Forward declaration
struct AllocHeader;

// ---------------------------------------------------------------------------
// CanaryFooter: 8-byte canary at the end of every block
// ---------------------------------------------------------------------------
// Defined before AllocHeader to avoid circular dependency.
// from_header() takes the block size explicitly rather than computing it from
// AllocHeader::block_size(), which would require the complete type.
struct alignas(8) CanaryFooter {
    uint32_t magic{0};
    uint32_t checksum{0};

    static void stamp(AllocHeader* header, size_t block_sz) noexcept;

    // block_sz: the total block size (user_size + overhead), from size_for_class()
    static CanaryFooter* from_header(AllocHeader* header, size_t block_sz) noexcept {
        return reinterpret_cast<CanaryFooter*>(
            reinterpret_cast<std::byte*>(header) + block_sz - sizeof(CanaryFooter));
    }

    static bool verify(const AllocHeader* header, size_t block_sz) noexcept {
        auto* f = from_header(const_cast<AllocHeader*>(header), block_sz);
        return f->magic == kAllocMagic;
    }
};

// ---------------------------------------------------------------------------
// AllocHeader: 32-byte metadata prefix on every block
// ---------------------------------------------------------------------------
struct alignas(32) AllocHeader {
    // owner_id when live; next when freed (freelist linkage via union)
    union {
        uint64_t owner_id;
        AllocHeader* next;
    };
    uint32_t incarnation{0};
    uint32_t magic{kAllocMagic};
    uint8_t  size_class{0};
    uint8_t  generation{0};
    uint16_t flags{0};
    uint32_t _padding{0};
    uint64_t timestamp{0};

    static AllocHeader* stamp(void* block, SizeClass sc, ActorId owner) noexcept {
        auto* h = static_cast<AllocHeader*>(block);
        h->owner_id = owner.value();
        h->incarnation = 0;
        h->magic = kAllocMagic;
        h->size_class = static_cast<uint8_t>(sc);
        h->generation = 0;
        h->flags = 0;
        h->_padding = 0;
        h->timestamp = 0;
        return h;
    }

    void* user_data() noexcept {
        return reinterpret_cast<std::byte*>(this) + sizeof(AllocHeader);
    }

    static AllocHeader* from_user_data(void* user_ptr) noexcept {
        return reinterpret_cast<AllocHeader*>(
            static_cast<std::byte*>(user_ptr) - sizeof(AllocHeader));
    }

    static std::byte* user_ptr(std::byte* block) noexcept {
        return block + sizeof(AllocHeader);
    }

    // Returns pointer to the CanaryFooter for this block
    std::byte* footer_ptr() const noexcept {
        return reinterpret_cast<std::byte*>(const_cast<AllocHeader*>(this))
               + block_size() - sizeof(CanaryFooter);
    }

    bool is_live() const noexcept { return magic == kAllocMagic; }
    bool is_freed() const noexcept { return magic == kFreedMagic; }

    size_t block_size() const noexcept {
        return mem::block_size(static_cast<SizeClass>(size_class));
    }

    size_t user_size() const noexcept {
        return mem::size_for_class(static_cast<SizeClass>(size_class));
    }
};

// Out-of-line because it needs AllocHeader complete type
inline void CanaryFooter::stamp(AllocHeader* header, size_t block_sz) noexcept {
    auto* f = from_header(header, block_sz);
    f->magic = kAllocMagic;
    f->checksum = 0;
}

} // namespace hpactor::mem
