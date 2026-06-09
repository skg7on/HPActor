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
#include <hpactor/mem/size_class.hpp>

using namespace hpactor::mem;

TEST(SizeClassTest, NumSizeClasses) {
    EXPECT_EQ(kNumSizeClasses, 8U);
}

TEST(SizeClassTest, EnumValuesArePowerOfTwoMultiples) {
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k32B), 0);
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k64B), 1);
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k128B), 2);
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k256B), 3);
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k512B), 4);
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k1KB), 5);
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k2KB), 6);
    EXPECT_EQ(static_cast<uint8_t>(SizeClass::k4KB), 7);
}

TEST(SizeClassTest, SizeForClass) {
    EXPECT_EQ(size_for_class(SizeClass::k32B), 32U);
    EXPECT_EQ(size_for_class(SizeClass::k64B), 64U);
    EXPECT_EQ(size_for_class(SizeClass::k128B), 128U);
    EXPECT_EQ(size_for_class(SizeClass::k256B), 256U);
    EXPECT_EQ(size_for_class(SizeClass::k512B), 512U);
    EXPECT_EQ(size_for_class(SizeClass::k1KB), 1024U);
    EXPECT_EQ(size_for_class(SizeClass::k2KB), 2048U);
    EXPECT_EQ(size_for_class(SizeClass::k4KB), 4096U);
}

TEST(SizeClassTest, ClassForSizeRoundsUpCorrectly) {
    EXPECT_EQ(class_for_size(1), SizeClass::k32B);
    EXPECT_EQ(class_for_size(32), SizeClass::k32B);
    EXPECT_EQ(class_for_size(33), SizeClass::k64B);
    EXPECT_EQ(class_for_size(64), SizeClass::k64B);
    EXPECT_EQ(class_for_size(65), SizeClass::k128B);
    EXPECT_EQ(class_for_size(128), SizeClass::k128B);
    EXPECT_EQ(class_for_size(400), SizeClass::k512B);
    EXPECT_EQ(class_for_size(500), SizeClass::k512B);
    EXPECT_EQ(class_for_size(513), SizeClass::k1KB);
    EXPECT_EQ(class_for_size(1024), SizeClass::k1KB);
    EXPECT_EQ(class_for_size(2000), SizeClass::k2KB);
    EXPECT_EQ(class_for_size(3000), SizeClass::k4KB);
    EXPECT_EQ(class_for_size(4096), SizeClass::k4KB);
}

TEST(SizeClassTest, BlockSizeIncludesHeaderAndFooterOverhead) {
    // Block sizes are rounded up to 32-byte alignment for AllocHeader.
    EXPECT_EQ(block_size(SizeClass::k32B), 96U);  // (32+40)=72 → round up to 96
    EXPECT_EQ(block_size(SizeClass::k64B), 128U); // (64+40)=104 → round up to
                                                  // 128
    EXPECT_EQ(block_size(SizeClass::k4KB), 4160U); // (4096+40)=4136 → round up
                                                   // to 4160
}

TEST(SizeClassTest, UserSizeSubtractsOverhead) {
    // user_size() handles the alignment padding via size-class lookup.
    EXPECT_EQ(user_size(block_size(SizeClass::k128B)), 128U);
}
