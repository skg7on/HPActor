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
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/core/mutex_mailbox.hpp>
#include <thread>
#include <vector>

using namespace hpactor;

TEST(MailboxStressTest, MultiProducerStress) {
    MutexMailbox<TypedMessage> mailbox;
    constexpr int num_threads = 100;
    constexpr int msgs_per_thread = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&mailbox, i]() {
            for (int j = 0; j < msgs_per_thread; ++j) {
                mailbox.push(TypedMessage(TypeTag::User,
                                          StreamBuffer{static_cast<uint8_t>(i),
                                                       static_cast<uint8_t>(j)}));
            }
        });
    }
    for (auto& t : threads)
        t.join();

    // Drain all messages
    int popped = 0;
    TypedMessage msg;
    while (mailbox.try_pop(msg)) {
        popped++;
    }

    EXPECT_EQ(popped, num_threads * msgs_per_thread);
}
