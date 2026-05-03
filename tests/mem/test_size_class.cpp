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

#include <hpactor/mem/size_class.hpp>

#include <cassert>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    // Verify 8 size classes
    assert(kNumSizeClasses == 8);

    // Verify size classes are power-of-two multiples of 32
    assert(static_cast<uint8_t>(SizeClass::k32B) == 0);
    assert(static_cast<uint8_t>(SizeClass::k64B) == 1);
    assert(static_cast<uint8_t>(SizeClass::k128B) == 2);
    assert(static_cast<uint8_t>(SizeClass::k256B) == 3);
    assert(static_cast<uint8_t>(SizeClass::k512B) == 4);
    assert(static_cast<uint8_t>(SizeClass::k1KB) == 5);
    assert(static_cast<uint8_t>(SizeClass::k2KB) == 6);
    assert(static_cast<uint8_t>(SizeClass::k4KB) == 7);

    // Verify size_for_class()
    assert(size_for_class(SizeClass::k32B) == 32);
    assert(size_for_class(SizeClass::k64B) == 64);
    assert(size_for_class(SizeClass::k128B) == 128);
    assert(size_for_class(SizeClass::k256B) == 256);
    assert(size_for_class(SizeClass::k512B) == 512);
    assert(size_for_class(SizeClass::k1KB) == 1024);
    assert(size_for_class(SizeClass::k2KB) == 2048);
    assert(size_for_class(SizeClass::k4KB) == 4096);

    // Verify class_for_size() rounds up correctly
    assert(class_for_size(1) == SizeClass::k32B);
    assert(class_for_size(32) == SizeClass::k32B);
    assert(class_for_size(33) == SizeClass::k64B);
    assert(class_for_size(64) == SizeClass::k64B);
    assert(class_for_size(65) == SizeClass::k128B);
    assert(class_for_size(128) == SizeClass::k128B);
    assert(class_for_size(400) == SizeClass::k512B);
    assert(class_for_size(500) == SizeClass::k512B);
    assert(class_for_size(513) == SizeClass::k1KB);
    assert(class_for_size(1024) == SizeClass::k1KB);
    assert(class_for_size(2000) == SizeClass::k2KB);
    assert(class_for_size(3000) == SizeClass::k4KB);
    assert(class_for_size(4096) == SizeClass::k4KB);

    // Verify block_size() includes header + footer overhead
    assert(block_size(SizeClass::k32B) == 32 + 40);   // 72
    assert(block_size(SizeClass::k64B) == 64 + 40);   // 104
    assert(block_size(SizeClass::k4KB) == 4096 + 40); // 4136

    // Verify user_size() subtracts overhead
    assert(user_size(block_size(SizeClass::k128B)) == 128);

    std::cout << "test_size_class: PASS\n";
    return 0;
}
