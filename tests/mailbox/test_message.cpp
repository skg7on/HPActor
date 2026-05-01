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
#include <hpactor/actor/typed_message.hpp>

int main() {
    // Test default construction
    hpactor::TypedMessage msg;
    assert(msg.type_id() == hpactor::TypeTag::Invalid);
    assert(msg.payload().empty());
    assert(msg.parsed() == nullptr);

    // Test construction from tag + payload
    hpactor::StreamBuffer data = {0x01, 0x02, 0x03};
    hpactor::TypedMessage msg2(hpactor::TypeTag::User, data);
    assert(msg2.type_id() == hpactor::TypeTag::User);
    assert(msg2.payload().size() == 3);

    // Test move semantics
    hpactor::TypedMessage msg3 = std::move(msg2);
    assert(msg3.type_id() == hpactor::TypeTag::User);
    assert(msg3.payload().size() == 3);

    return 0;
}
