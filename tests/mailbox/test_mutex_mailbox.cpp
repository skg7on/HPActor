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
#include <hpactor/core/mutex_mailbox.hpp>
#include <thread>
#include <vector>

using namespace hpactor;

int main() {
    MutexMailbox<TypedMessage> mailbox;

    // Test push/pop
    mailbox.push(TypedMessage(TypeTag::User, bytes{42}));
    assert(mailbox.size() == 1);

    TypedMessage msg;
    bool popped = mailbox.pop(msg);
    assert(popped);
    assert(msg.type_id() == TypeTag::User);
    assert(msg.payload().size() == 1);
    assert(msg.payload()[0] == 42);
    assert(mailbox.empty());

    // Test try_pop on empty
    bool tried = mailbox.try_pop(msg);
    assert(!tried);

    // Test thread safety - push from multiple threads
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&mailbox, i]() {
            for (int j = 0; j < 100; ++j) {
                mailbox.push(TypedMessage(TypeTag::User,
                    bytes{static_cast<uint8_t>(i), static_cast<uint8_t>(j)}));
            }
        });
    }
    for (auto& t : threads)
        t.join();

    assert(mailbox.size() == 1000);

    return 0;
}