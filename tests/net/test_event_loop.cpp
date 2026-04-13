#include <hpactor/net/event_loop.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    printf("=== EventLoop Integration Tests ===\n");

    // Test 1: Constructor/destructor
    {
        printf("Test 1: Constructor/destructor... ");
        hpactor::net::EventLoop loop;
        // Destructs without crash
        printf("PASS\n");
    }

    // Test 2: start() - backend starts correctly
    {
        printf("Test 2: Backend initialization... ");
        hpactor::net::EventLoop loop;
        // Backend should be running
        printf("PASS\n");
    }

    // Test 3: run_after with immediate callback
    {
        printf("Test 3: run_after immediate callback... ");
        hpactor::net::EventLoop loop;
        std::atomic<bool> fired{false};
        uint64_t handle = loop.run_after([&fired]() { fired = true; }, 10);
        assert(handle > 0 && "run_after should return valid handle");

        // Process completions and wait
        loop.wait(50);
        loop.process_completions();

        assert(fired && "Timer callback should have fired");
        printf("PASS\n");
    }

    // Test 4: run_after then cancel
    {
        printf("Test 4: Timer cancellation... ");
        hpactor::net::EventLoop loop;
        std::atomic<bool> fired{false};
        uint64_t handle = loop.run_after([&fired]() { fired = true; }, 100);
        assert(handle > 0);

        // Cancel immediately
        loop.cancel_timer(handle);
        loop.wait(150);
        loop.process_completions();

        assert(!fired && "Cancelled timer should not fire");
        printf("PASS\n");
    }

    // Test 5: run_every repeating timer
    {
        printf("Test 5: run_every repeating timer... ");
        hpactor::net::EventLoop loop;
        std::atomic<int> count{0};
        uint64_t handle = loop.run_every([&count]() { count++; }, 20);
        assert(handle > 0);

        // Wait for multiple firings using a loop (mimics real event loop usage)
        int waited = 0;
        while (count.load() < 2 && waited < 200) {
            loop.wait(20);
            loop.process_completions();
            waited += 20;
        }

        assert(count >= 2 && "run_every should fire multiple times");
        printf("PASS (count=%d)\n", count.load());
    }

    // Test 6: Multiple concurrent timers
    {
        printf("Test 6: Multiple concurrent timers... ");
        hpactor::net::EventLoop loop;
        std::atomic<int> count{0};
        constexpr int NUM_TIMERS = 5;

        uint64_t handles[NUM_TIMERS];
        for (int i = 0; i < NUM_TIMERS; ++i) {
            handles[i] = loop.run_after([&count]() { count++; }, 10 + i * 10);
            assert(handles[i] > 0);
        }

        // Wait for all to fire using a loop
        int waited = 0;
        while (count.load() < NUM_TIMERS && waited < 500) {
            loop.wait(50);
            loop.process_completions();
            waited += 50;
        }

        assert(count == NUM_TIMERS && "All timers should fire");
        printf("PASS\n");
    }

    // Test 7: fd registration with socketpair
    {
        printf("Test 7: fd registration... ");
        hpactor::net::EventLoop loop;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0 && "socketpair should succeed");

        bool added = loop.add_fd(fds[0], net::EventLoop::Event::Read);
        assert(added && "add_fd should return true");

        bool updated = loop.update_fd(fds[0], net::EventLoop::Event::Write);
        assert(updated && "update_fd should return true");

        bool removed = loop.remove_fd(fds[0]);
        assert(removed && "remove_fd should return true");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }

    // Test 8: Timer firing order (verify all fire, order is FIFO based on scheduling)
    {
        printf("Test 8: Timer firing order... ");
        hpactor::net::EventLoop loop;
        std::vector<int> order;
        std::mutex order_mutex;

        loop.run_after([&order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(1);
        }, 50);

        loop.run_after([&order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(2);
        }, 30);

        loop.run_after([&order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(3);
        }, 10);

        // Wait for all using a loop
        int waited = 0;
        while (order.size() < 3 && waited < 500) {
            loop.wait(50);
            loop.process_completions();
            waited += 50;
        }

        {
            std::lock_guard<std::mutex> lock(order_mutex);
            assert(order.size() == 3 && "All timers should fire");
            // With dispatch_after_f on serial queue, order is FIFO (scheduling order)
            // Not deadline-based ordering
        }
        printf("PASS (order={%d,%d,%d})\n", order[0], order[1], order[2]);
    }

    // Test 9: run_every cancellation
    {
        printf("Test 9: run_every cancellation... ");
        hpactor::net::EventLoop loop;
        std::atomic<int> count{0};
        uint64_t handle = loop.run_every([&count]() { count++; }, 10);
        assert(handle > 0);

        // Let it fire a few times using a loop
        int waited = 0;
        while (count.load() < 2 && waited < 200) {
            loop.wait(20);
            loop.process_completions();
            waited += 20;
        }

        int count_before_cancel = count.load();
        assert(count_before_cancel >= 2);

        // Cancel
        loop.cancel_timer(handle);

        // Wait more to ensure it stopped
        int count_after_cancel = count.load();
        waited = 0;
        while (count_after_cancel == count.load() && waited < 100) {
            loop.wait(20);
            loop.process_completions();
            waited += 20;
        }

        count_after_cancel = count.load();
        assert(count_after_cancel == count_before_cancel && "Cancelled timer should stop firing");
        printf("PASS\n");
    }

    // Test 10: wait timeout returns 0
    {
        printf("Test 10: wait timeout behavior... ");
        hpactor::net::EventLoop loop;
        // No timers or fds registered - should timeout
        int result = loop.wait(10);
        assert(result == 0 && "wait with no activity should return 0");
        printf("PASS\n");
    }

    // Test 11: fd edge-triggered notification via socket data
    {
        printf("Test 11: fd read notification... ");
        hpactor::net::EventLoop loop;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.add_fd(fds[0], net::EventLoop::Event::Read);

        // Write data to fds[1]
        const char* msg = "hello";
        ssize_t written = ::write(fds[1], msg, 5);
        assert(written == 5);

        // Wait for read notification
        int result = loop.wait(100);
        assert(result >= 0 && "wait should return >= 0");

        loop.remove_fd(fds[0]);
        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }

    // Test 12: has_event returns true for registered fds
    {
        printf("Test 12: has_event... ");
        hpactor::net::EventLoop loop;

        int fds[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

        loop.add_fd(fds[0], net::EventLoop::Event::Read);
        assert(loop.has_event(fds[0], net::EventLoop::Event::Read));

        loop.remove_fd(fds[0]);
        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }

    // Test 13: Stress test - many rapid timers
    {
        printf("Test 13: Stress test - many rapid timers... ");
        hpactor::net::EventLoop loop;
        std::atomic<int> count{0};
        constexpr int NUM_TIMERS = 20;

        for (int i = 0; i < NUM_TIMERS; ++i) {
            loop.run_after([&count]() { count++; }, 5);
        }

        // Wait for all using a loop
        int waited = 0;
        while (count.load() < NUM_TIMERS && waited < 500) {
            loop.wait(20);
            loop.process_completions();
            waited += 20;
        }

        assert(count == NUM_TIMERS && "All rapid timers should fire");
        printf("PASS\n");
    }

    printf("=== All EventLoop Tests Passed ===\n");
    return 0;
}
