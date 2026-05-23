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

TEST(MutexMailboxTest, PushAndPop) {
    MutexMailbox<TypedMessage> mailbox;

    mailbox.push(TypedMessage(TypeTag::User, StreamBuffer{42}));
    EXPECT_EQ(mailbox.size(), 1);

    TypedMessage msg;
    bool popped = mailbox.pop(msg);
    EXPECT_TRUE(popped);
    EXPECT_EQ(msg.type_id(), TypeTag::User);
    EXPECT_EQ(msg.payload().size(), 1);
    EXPECT_EQ(msg.payload()[0], 42);
    EXPECT_TRUE(mailbox.empty());
}

TEST(MutexMailboxTest, TryPopOnEmpty) {
    MutexMailbox<TypedMessage> mailbox;
    TypedMessage msg;
    bool tried = mailbox.try_pop(msg);
    EXPECT_FALSE(tried);
}

TEST(MutexMailboxTest, ThreadSafetyMultiProducer) {
    MutexMailbox<TypedMessage> mailbox;

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&mailbox, i]() {
            for (int j = 0; j < 100; ++j) {
                mailbox.push(TypedMessage(TypeTag::User,
                                          StreamBuffer{static_cast<uint8_t>(i),
                                                       static_cast<uint8_t>(j)}));
            }
        });
    }
    for (auto& t : threads)
        t.join();

    EXPECT_EQ(mailbox.size(), 1000);
}
