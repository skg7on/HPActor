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

#include <cassert>
#include <hpactor/actor/message.hpp>
#include <string>

struct MoveOnly {
    int value;
    std::string data;
    MoveOnly() = default;
    MoveOnly(int v, std::string d) : value(v), data(std::move(d)) {}
    MoveOnly(MoveOnly&& other) noexcept
        : value(other.value), data(std::move(other.data)) {}
    MoveOnly& operator=(MoveOnly&& other) noexcept {
        value = other.value;
        data = std::move(other.data);
        return *this;
    }
    // Delete copy
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
};

int main() {
    // Test move-only type with rvalue (move constructor)
    MoveOnly m{42, "test"};
    hpactor::Message<MoveOnly> msg{std::move(m)};
    assert(msg.payload().value == 42);
    // Verify original moved-from state
    assert(m.value == 42); // int copied, string moved-from

    // Test move-only type by constructing MoveOnly first, then wrapping
    hpactor::Message<MoveOnly> msg2{MoveOnly{100, "moved"}};
    assert(msg2.payload().value == 100);

    return 0;
}