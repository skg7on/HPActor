#include <hpactor/mutex_mailbox.hpp>
#include <hpactor/message.hpp>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>

struct StressMsg {
    int value;
    char padding[60];  // Cache line padding
};

int main() {
    hpactor::MutexMailbox<StressMsg> mailbox;
    std::atomic<int> count{0};
    constexpr int num_threads = 100;
    constexpr int msgs_per_thread = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&mailbox, &count, i]() {
            for (int j = 0; j < msgs_per_thread; ++j) {
                mailbox.push(hpactor::Message<StressMsg>{StressMsg{i * 1000 + j, {}}});
                count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();

    // Drain all messages
    int popped = 0;
    hpactor::Message<StressMsg> msg;
    while (mailbox.try_pop(msg)) {
        popped++;
    }

    assert(popped == num_threads * msgs_per_thread);
    printf("Stress test passed: %d messages from %d threads\n", popped, num_threads);

    return 0;
}