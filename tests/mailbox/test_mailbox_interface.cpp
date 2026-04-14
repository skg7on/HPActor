// tests/test_mailbox_interface.cpp
#include <cassert>
#include <hpactor/core/mailbox.hpp>
#include <hpactor/actor/message.hpp>
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