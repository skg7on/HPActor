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
#include <hpactor/mem/size_class.hpp>

#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    // Verify sizes
    static_assert(sizeof(AllocHeader) == 32, "AllocHeader must be 32 bytes");
    static_assert(sizeof(CanaryFooter) == 8, "CanaryFooter must be 8 bytes");
    assert(sizeof(AllocHeader) == 32);
    assert(sizeof(CanaryFooter) == 8);

    // Allocate a raw buffer and stamp it
    constexpr size_t bs = block_size(SizeClass::k128B); // 128 + 40 = 168
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    // Stamp header
    AllocHeader* hdr = AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42});
    assert(hdr != nullptr);
    assert(hdr->owner_id == 42);
    assert(hdr->magic == kAllocMagic);
    assert(hdr->size_class == static_cast<uint8_t>(SizeClass::k128B));
    assert(hdr->generation == 0);
    assert(hdr->flags == 0);

    // Verify user_data() returns pointer after header
    void* user = hdr->user_data();
    assert(static_cast<std::byte*>(user) == buffer + sizeof(AllocHeader));

    // Stamp footer
    CanaryFooter::stamp(hdr, bs);
    CanaryFooter* ftr = CanaryFooter::from_header(hdr, bs);
    assert(ftr->magic == kAllocMagic);

    // Verify footer is at the end of the block
    assert(reinterpret_cast<std::byte*>(ftr)
           == buffer + bs - sizeof(CanaryFooter));

    // Verify canary
    assert(CanaryFooter::verify(hdr, bs) == true);

    // Corrupt the canary and verify detection
    ftr->magic = 0xDEAD;
    assert(CanaryFooter::verify(hdr, bs) == false);

    // Test from_user_data() round-trip
    AllocHeader* hdr2 = AllocHeader::from_user_data(user);
    assert(hdr2 == hdr);
    assert(hdr2->owner_id == 42);

    // Test freed magic
    hdr->magic = kFreedMagic;
    assert(hdr->is_freed() == true);
    assert(hdr->is_live() == false);

    // Test freelist linkage via union (next valid when freed)
    AllocHeader dummy;
    hdr->next = &dummy;
    assert(hdr->next == &dummy);

    // Test footer_ptr helper
    assert(hdr->footer_ptr() == reinterpret_cast<std::byte*>(ftr));
    assert(AllocHeader::user_ptr(buffer) == static_cast<std::byte*>(user));

    std::cout << "test_alloc_header: PASS\n";
    return 0;
}
