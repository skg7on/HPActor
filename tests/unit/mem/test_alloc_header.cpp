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

#include <gtest/gtest.h>
#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/size_class.hpp>

#include <cstring>

using namespace hpactor::mem;

TEST(AllocHeaderTest, Sizes) {
    static_assert(sizeof(AllocHeader) == 32, "AllocHeader must be 32 bytes");
    static_assert(sizeof(CanaryFooter) == 8, "CanaryFooter must be 8 bytes");
    EXPECT_EQ(sizeof(AllocHeader), 32U);
    EXPECT_EQ(sizeof(CanaryFooter), 8U);
}

TEST(AllocHeaderTest, StampHeader) {
    constexpr size_t bs = block_size(SizeClass::k128B); // 128 + 40 = 168
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    AllocHeader* hdr =
        AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->owner_id, 42U);
    EXPECT_EQ(hdr->magic, kAllocMagic);
    EXPECT_EQ(hdr->size_class, static_cast<uint8_t>(SizeClass::k128B));
    EXPECT_EQ(hdr->generation, 0U);
    EXPECT_EQ(hdr->flags, 0U);
}

TEST(AllocHeaderTest, UserDataReturnsPointerAfterHeader) {
    constexpr size_t bs = block_size(SizeClass::k128B);
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    AllocHeader* hdr =
        AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    void* user = hdr->user_data();
    EXPECT_EQ(static_cast<std::byte*>(user), buffer + sizeof(AllocHeader));
}

TEST(AllocHeaderTest, StampFooterAndCanary) {
    constexpr size_t bs = block_size(SizeClass::k128B);
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    AllocHeader* hdr =
        AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    CanaryFooter::stamp(hdr, bs);
    CanaryFooter* ftr = CanaryFooter::from_header(hdr, bs);
    EXPECT_EQ(ftr->magic, kAllocMagic);

    // Verify footer is at the end of the block
    EXPECT_EQ(reinterpret_cast<std::byte*>(ftr), buffer + bs - sizeof(CanaryFooter));
}

TEST(AllocHeaderTest, CanaryVerification) {
    constexpr size_t bs = block_size(SizeClass::k128B);
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    AllocHeader* hdr =
        AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    CanaryFooter::stamp(hdr, bs);
    CanaryFooter* ftr = CanaryFooter::from_header(hdr, bs);

    EXPECT_TRUE(CanaryFooter::verify(hdr, bs));

    // Corrupt the canary and verify detection
    ftr->magic = 0xDEAD;
    EXPECT_FALSE(CanaryFooter::verify(hdr, bs));
}

TEST(AllocHeaderTest, FromUserDataRoundTrip) {
    constexpr size_t bs = block_size(SizeClass::k128B);
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    AllocHeader* hdr =
        AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    void* user = hdr->user_data();

    AllocHeader* hdr2 = AllocHeader::from_user_data(user);
    EXPECT_EQ(hdr2, hdr);
    EXPECT_EQ(hdr2->owner_id, 42U);
}

TEST(AllocHeaderTest, FreedMagic) {
    constexpr size_t bs = block_size(SizeClass::k128B);
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    AllocHeader* hdr =
        AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    hdr->magic = kFreedMagic;
    EXPECT_TRUE(hdr->is_freed());
    EXPECT_FALSE(hdr->is_live());
}

TEST(AllocHeaderTest, FreelistLinkageViaUnion) {
    AllocHeader hdr;
    AllocHeader dummy;
    hdr.next = &dummy;
    EXPECT_EQ(hdr.next, &dummy);
}

TEST(AllocHeaderTest, FooterPtrAndUserPtrHelpers) {
    constexpr size_t bs = block_size(SizeClass::k128B);
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    AllocHeader* hdr =
        AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    CanaryFooter::stamp(hdr, bs);
    CanaryFooter* ftr = CanaryFooter::from_header(hdr, bs);

    EXPECT_EQ(hdr->footer_ptr(), reinterpret_cast<std::byte*>(ftr));
    EXPECT_EQ(AllocHeader::user_ptr(buffer), hdr->user_data());
}
