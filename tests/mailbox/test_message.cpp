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

struct TestPayload {
    int value;
    std::string data;
};

int main() {
    // Test default construction
    hpactor::Message<TestPayload> msg;
    // Test with payload
    hpactor::Message<TestPayload> msg2{TestPayload{42, "hello"}};
    assert(msg2.payload().value == 42);
    assert(msg2.payload().data == "hello");
    // Test move semantics
    TestPayload p{100, "moved"};
    hpactor::Message<TestPayload> msg3{std::move(p)};
    assert(msg3.payload().value == 100);
    return 0;
}