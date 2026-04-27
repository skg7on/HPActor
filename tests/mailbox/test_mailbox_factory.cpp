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
#include <hpactor/core/mailbox.hpp>
#include <hpactor/core/mutex_mailbox.hpp>

using namespace hpactor;

int main() {
    auto mailbox = create_mailbox<TypedMessage, MailboxType::Mutex>();
    mailbox->push(TypedMessage(TypeTag::User, bytes{1, 2, 3}));
    assert(mailbox->size() == 1);
    return 0;
}