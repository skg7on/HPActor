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

#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/types/types.hpp>

#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

/// \brief Magic value written into live AllocHeader and CanaryFooter.
inline constexpr uint32_t kAllocMagic = 0xAC70AC70;

/// \brief Magic value written into freed blocks for debug detection of
/// use-after-free.
inline constexpr uint32_t kFreedMagic = 0xDEADDEAD;

/// \brief Bitmask extracting the RegionType from the flags field.
inline constexpr uint16_t kAllocRegionMask = 0x0007;

/// \brief Flag bit indicating this block was allocated via the fallback path
/// (e.g. std::malloc for oversized blocks).
inline constexpr uint16_t kAllocFallbackFlag = 0x0008;

/// \brief Encode a RegionType into the flags field, masking to the region bits.
///
/// \param[in] region The region to encode.
/// \return Flags value with only the region bits set.
inline constexpr uint16_t flags_for_region(RegionType region) noexcept {
    return static_cast<uint16_t>(region) & kAllocRegionMask;
}

// Forward declaration
struct AllocHeader;

// ---------------------------------------------------------------------------
// CanaryFooter: 8-byte canary at the end of every block
// ---------------------------------------------------------------------------
// Defined before AllocHeader to avoid circular dependency.
// from_header() takes the block size explicitly rather than computing it from
// AllocHeader::block_size(), which would require the complete type.

/// \brief 8-byte canary footer placed at the end of every managed block.
///
/// Used in debug builds (\c ENABLE_MEMORY_DEBUG) to detect buffer overflows.
/// The footer is stamped with \c kAllocMagic on allocation and verified on
/// free.
struct alignas(8) CanaryFooter {
    uint32_t magic{0};    ///< Must equal \c kAllocMagic for a valid footer.
    uint32_t checksum{0}; ///< Reserved for future checksum; currently always 0.

    /// \brief Write the canary magic into the footer for a freshly allocated
    /// block.
    ///
    /// \param[in,out] header The block's AllocHeader.
    /// \param[in] block_sz Total block size (user_size + overhead), from
    ///            \c block_size() for the size class.
    static void stamp(AllocHeader* header, size_t block_sz) noexcept;

    /// \brief Compute the CanaryFooter address from the block's AllocHeader.
    ///
    /// \param[in] header The block's AllocHeader.
    /// \param[in] block_sz Total block size.
    /// \return Pointer to the CanaryFooter at the end of the block.
    static CanaryFooter* from_header(AllocHeader* header, size_t block_sz) noexcept {
        auto* base = reinterpret_cast<char*>(header);
        return reinterpret_cast<CanaryFooter*>(base + block_sz -
                                               sizeof(CanaryFooter));
    }

    /// \brief Verify the canary magic is intact.
    ///
    /// \param[in] header The block's AllocHeader.
    /// \param[in] block_sz Total block size.
    /// \return \c true if the canary contains \c kAllocMagic.
    static bool verify(const AllocHeader* header, size_t block_sz) noexcept {
        auto* f = from_header(const_cast<AllocHeader*>(header), block_sz);
        return f->magic == kAllocMagic;
    }
};

// ---------------------------------------------------------------------------
// AllocHeader: 32-byte metadata prefix on every block
// ---------------------------------------------------------------------------

/// \brief 32-byte metadata prefix placed before every managed block.
///
/// Stores ownership, size class, generation, and region provenance.
/// The fields are packed to fit in a single 32-byte aligned prefix; the
/// user data payload starts immediately after this header.
struct alignas(32) AllocHeader {
    /// \brief Owner actor ID when live; freelist \c next pointer when freed.
    union {
        uint64_t owner_id;
        AllocHeader* next;
    };
    uint32_t incarnation{0};     ///< Monotonic counter incremented on each
                                 ///< free/realloc.
    uint32_t magic{kAllocMagic}; ///< \c kAllocMagic for live, \c kFreedMagic
                                 ///< for freed.
    uint8_t size_class{0};       ///< SizeClass index this block belongs to.
    uint8_t generation{0};       ///< Compaction generation counter.
    uint16_t flags{0};           ///< Region type and fallback flag (see \c
                                 ///< kAllocRegionMask).
    uint32_t _padding{0};        ///< Reserved padding.
    uint64_t timestamp{0};       ///< Allocation timestamp (monotonic ns).

    /// \brief Initialize a freshly allocated block's header.
    ///
    /// \param[in] block Raw block pointer (points to the AllocHeader).
    /// \param[in] sc Size class of this block.
    /// \param[in] owner Owning actor.
    /// \param[in] region Memory region for provenance (default kInternal).
    /// \param[in] fallback \c true if allocated via the fallback path.
    /// \return Pointer to the initialized AllocHeader.
    static AllocHeader* stamp(void* block, SizeClass sc, ActorId owner,
                              RegionType region = RegionType::kInternal,
                              bool fallback = false) noexcept {
        auto* h = static_cast<AllocHeader*>(block);
        h->owner_id = owner.value();
        h->incarnation = 0;
        h->magic = kAllocMagic;
        h->size_class = static_cast<uint8_t>(sc);
        h->generation = 0;
        h->flags = flags_for_region(region);
        if (fallback) {
            h->flags |= kAllocFallbackFlag;
        }
        h->_padding = 0;
        h->timestamp = 0;
        return h;
    }

    /// \brief Return a pointer to the user data payload following this header.
    ///
    /// \return Pointer to the first user-usable byte.
    void* user_data() noexcept {
        return reinterpret_cast<char*>(this) + sizeof(AllocHeader);
    }

    /// \brief Recover the AllocHeader pointer from a user data pointer.
    ///
    /// \param[in] user_ptr Pointer previously returned by \c user_data().
    /// \return Pointer to the AllocHeader preceding the user data.
    static AllocHeader* from_user_data(void* user_ptr) noexcept {
        return reinterpret_cast<AllocHeader*>(static_cast<char*>(user_ptr) -
                                              sizeof(AllocHeader));
    }

    /// \brief Compute the user data pointer from a raw block pointer.
    ///
    /// \param[in] block Raw block pointer (points to the AllocHeader).
    /// \return \c std::byte pointer to the start of user data.
    static std::byte* user_ptr(std::byte* block) noexcept {
        return reinterpret_cast<std::byte*>(reinterpret_cast<char*>(block) +
                                            sizeof(AllocHeader));
    }

    /// \brief Return a pointer to the CanaryFooter for this block.
    ///
    /// \return Pointer to the footer at the end of the block.
    std::byte* footer_ptr() const noexcept {
        auto* base = reinterpret_cast<char*>(const_cast<AllocHeader*>(this));
        return reinterpret_cast<std::byte*>(base + block_size() -
                                            sizeof(CanaryFooter));
    }

    /// \brief Check whether this block is currently live (not freed).
    ///
    /// \return \c true if the magic field is \c kAllocMagic.
    bool is_live() const noexcept {
        return magic == kAllocMagic;
    }

    /// \brief Check whether this block is freed.
    ///
    /// \return \c true if the magic field is \c kFreedMagic.
    bool is_freed() const noexcept {
        return magic == kFreedMagic;
    }

    /// \brief Return the total block size (user data + overhead).
    ///
    /// \return Size in bytes for the block's size class.
    size_t block_size() const noexcept {
        return mem::block_size(static_cast<SizeClass>(size_class));
    }

    /// \brief Return the user-payload size for this block's size class.
    ///
    /// \return Usable bytes available to the caller.
    size_t user_size() const noexcept {
        return mem::size_for_class(static_cast<SizeClass>(size_class));
    }

    /// \brief Return the memory region this block is charged to.
    ///
    /// \return The RegionType encoded in the flags field.
    RegionType region() const noexcept {
        return static_cast<RegionType>(flags & kAllocRegionMask);
    }

    /// \brief Update the region provenance for this block.
    ///
    /// \param[in] region The new RegionType to record.
    void set_region(RegionType region) noexcept {
        flags = static_cast<uint16_t>((flags & ~kAllocRegionMask) |
                                      flags_for_region(region));
    }

    /// \brief Check whether this block was allocated via the fallback path.
    ///
    /// \return \c true if the fallback flag is set.
    bool is_fallback() const noexcept {
        return (flags & kAllocFallbackFlag) != 0;
    }

    /// \brief Mark this block as having been allocated via the fallback path.
    void mark_fallback() noexcept {
        flags |= kAllocFallbackFlag;
    }
};

// Out-of-line because it needs AllocHeader complete type
inline void CanaryFooter::stamp(AllocHeader* header, size_t block_sz) noexcept {
    auto* f = from_header(header, block_sz);
    f->magic = kAllocMagic;
    f->checksum = 0;
}

} // namespace hpactor::mem
