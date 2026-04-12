#include <cassert>
#include <hpactor/message.hpp>
#include <hpactor/mutex_mailbox.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace hpactor;

struct PingMsg {
    int value;
};

int main() {
    hpactor::MutexMailbox<PingMsg> mailbox;

    // Test push/pop
    mailbox.push(Message<PingMsg>{PingMsg{42}});
    assert(mailbox.size() == 1);

    hpactor::Message<PingMsg> msg;
    bool popped = mailbox.pop(msg);
    assert(popped);
    assert(msg.payload().value == 42);
    assert(mailbox.empty());

    // Test try_pop on empty
    bool tried = mailbox.try_pop(msg);
    assert(!tried); // Should return false

    // Test thread safety - push from multiple threads
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&mailbox, i]() {
            for (int j = 0; j < 100; ++j) {
                mailbox.push(Message<PingMsg>{PingMsg{i * 100 + j}});
            }
        });
    }
    for (auto& t : threads)
        t.join();

    assert(mailbox.size() == 1000); // All messages delivered

    return 0;
}