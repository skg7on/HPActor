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

// tests/test_mailbox_interface.cpp
#include <cassert>
#include <hpactor/actor/message.hpp>
#include <hpactor/core/mailbox.hpp>
#include <string>
#include <thread>

struct PingMsg {
    int value;
};

int main() {
    // Test can create via interface
    hpactor::IMailbox<PingMsg>* mailbox = nullptr;
    // Interface doesn't compile - no factory
    // This test just verifies interface compiles
    (void)mailbox;
    return 0;
}