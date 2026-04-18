# Event Loop Async I/O Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add async_send, async_recv, async_sendto, async_recvfrom tests to EventLoop using kqueue backend on macOS.

**Architecture:** Add optional completion_callback to EventLoop for test verification. Tests use socketpair(AF_UNIX, SOCK_STREAM) for connected socket tests, call backend()->async_*() directly, then process_completions() and verify via callback.

**Tech Stack:** C++20, kqueue backend, socketpair for connected sockets

---

## File Structure

```
include/hpactor/net/event_loop.hpp    # Add completion_callback setter
src/net/event_loop.cpp                # Implement completion_callback
tests/net/test_event_loop.cpp         # Append 8 async I/O tests
```

---

## Task 1: Add completion_callback to EventLoop

**Files:**
- Modify: `include/hpactor/net/event_loop.hpp` (find `ActorSystem* actor_system_` member)
- Modify: `src/net/event_loop.cpp` (find `enqueue_completion` method)

- [ ] **Step 1: Add completion_callback member and setter to EventLoop header**

Open `include/hpactor/net/event_loop.hpp` and add before the private member `ActorSystem* actor_system_` (around line 139):

```cpp
// Optional callback for test verification (disabled in production)
using completion_callback = std::function<void(OpCompletion)>;
void set_completion_callback(completion_callback cb) {
    completion_callback_ = std::move(cb);
}
```

Add member variable before `ActorSystem* actor_system_`:
```cpp
completion_callback completion_callback_;
```

- [ ] **Step 2: Modify enqueue_completion to invoke callback if set**

Open `src/net/event_loop.cpp` and find the `enqueue_completion` method. Replace it with:

```cpp
void EventLoop::enqueue_completion(OpCompletion completion) {
    if (completion_callback_) {
        completion_callback_(completion);
        return;
    }
    if (completion.type == OpType::TimerFired) {
        deliver_timer_completion(completion);
    } else if (actor_system_) {
        actor_system_->enqueue_completion(completion);
    }
}
```

- [ ] **Step 3: Build to verify compilation**

Run: `ninja -C build`
Expected: Builds without errors

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/event_loop.hpp src/net/event_loop.cpp
git commit -m "feat(event_loop): add optional completion_callback for test verification

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 2: Add async_send test

**Files:**
- Modify: `tests/net/test_event_loop.cpp:863-867`

- [ ] **Step 1: Add async_send test to test_event_loop.cpp**

Insert after line 863 (before the final `printf("=== All EventLoop Tests Passed ===\n");`):

```cpp
    // Test 26: async_send on socketpair
    {
        printf("Test 26: async_send... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0 && "socketpair should succeed");

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();
        assert(backend != nullptr && "backend should exist");

        struct iovec iov;
        char send_buf[] = "hello";
        iov.iov_base = send_buf;
        iov.iov_len = 5;

        backend->async_send(fds[0], &iov, 1, ActorId(1), static_cast<uint32_t>(OpType::Send));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result == 5 && "async_send should send 5 bytes");
        assert(captured->type == OpType::Send && "completion type should be Send");
        assert(captured->fd == fds[0] && "completion fd should match");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run the test**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Test 26 PASS (along with existing tests 1-13)

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add async_send test on socketpair

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 3: Add async_recv test

**Files:**
- Modify: `tests/net/test_event_loop.cpp`

- [ ] **Step 1: Add async_recv test**

Insert after Test 26:

```cpp
    // Test 27: async_recv on socketpair
    {
        printf("Test 27: async_recv... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();

        // Write data to fd[1] first
        const char* msg = "hello";
        ssize_t written = ::write(fds[1], msg, 5);
        assert(written == 5);

        char recv_buf[64];
        struct iovec iov;
        iov.iov_base = recv_buf;
        iov.iov_len = 64;

        backend->async_recv(fds[0], &iov, 1, ActorId(1), static_cast<uint32_t>(OpType::Recv));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result == 5 && "async_recv should receive 5 bytes");
        assert(captured->type == OpType::Recv && "completion type should be Recv");
        assert(memcmp(recv_buf, "hello", 5) == 0 && "received data should match");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Tests 26-27 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add async_recv test on socketpair

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 4: Add async_sendto test

**Files:**
- Modify: `tests/net/test_event_loop.cpp`

- [ ] **Step 1: Add async_sendto test**

Insert after Test 27:

```cpp
    // Test 28: async_sendto on socketpair (address ignored for connected sockets)
    {
        printf("Test 28: async_sendto... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();

        struct iovec iov;
        char send_buf[] = "test";
        iov.iov_base = send_buf;
        iov.iov_len = 4;

        // Address is ignored on connected sockets but we provide valid address
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/tmp/test", sizeof(addr.sun_path) - 1);

        backend->async_sendto(fds[0], &iov, 1, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr), ActorId(1), static_cast<uint32_t>(OpType::SendTo));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result == 4 && "async_sendto should send 4 bytes");
        assert(captured->type == OpType::SendTo && "completion type should be SendTo");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Tests 26-28 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add async_sendto test on socketpair

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 5: Add async_recvfrom test

**Files:**
- Modify: `tests/net/test_event_loop.cpp`

- [ ] **Step 1: Add async_recvfrom test**

Insert after Test 28:

```cpp
    // Test 29: async_recvfrom on socketpair
    {
        printf("Test 29: async_recvfrom... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();

        // Write data to fd[1] first
        const char* msg = "world";
        ssize_t written = ::write(fds[1], msg, 5);
        assert(written == 5);

        char recv_buf[64];
        struct iovec iov;
        iov.iov_base = recv_buf;
        iov.iov_len = 64;

        backend->async_recvfrom(fds[0], &iov, 1, ActorId(1), static_cast<uint32_t>(OpType::RecvFrom));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result == 5 && "async_recvfrom should receive 5 bytes");
        assert(captured->type == OpType::RecvFrom && "completion type should be RecvFrom");
        assert(memcmp(recv_buf, "world", 5) == 0 && "received data should match");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Tests 26-29 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add async_recvfrom test on socketpair

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 6: Add error handling test - async_send on closed fd

**Files:**
- Modify: `tests/net/test_event_loop.cpp`

- [ ] **Step 1: Add error test for async_send on closed fd**

Insert after Test 29:

```cpp
    // Test 30: async_send error on closed fd
    {
        printf("Test 30: async_send error on closed fd... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();

        // Close fd[0] before async_send
        ::close(fds[0]);
        // fds[0] is now closed

        struct iovec iov;
        char send_buf[] = "hello";
        iov.iov_base = send_buf;
        iov.iov_len = 5;

        backend->async_send(fds[0], &iov, 1, ActorId(1), static_cast<uint32_t>(OpType::Send));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result < 0 && "async_send on closed fd should return error");
        // EBADF = 9

        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Tests 26-30 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add async_send error test on closed fd

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 7: Add async_recv empty buffer test

**Files:**
- Modify: `tests/net/test_event_loop.cpp`

- [ ] **Step 1: Add async_recv empty buffer test**

Insert after Test 30:

```cpp
    // Test 31: async_recv with zero-length iovec (edge case)
    {
        printf("Test 31: async_recv empty buffer... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();

        // Call async_recv with zero-length read (just to trigger completion)
        // On connected sockets, read with iov_len=0 still triggers notification
        struct iovec iov;
        char buf[1];  // not used since iov_len = 0
        iov.iov_base = buf;
        iov.iov_len = 0;  // zero-length read

        backend->async_recv(fds[0], &iov, 1, ActorId(1), static_cast<uint32_t>(OpType::Recv));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        // With iov_len=0, should return 0 (EOF or immediate completion)
        assert(captured->result == 0 && "async_recv with empty buffer should return 0");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Tests 26-31 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add async_recv empty buffer edge case test

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 8: Add multi-iovec async_send test

**Files:**
- Modify: `tests/net/test_event_loop.cpp`

- [ ] **Step 1: Add multi-iovec async_send test**

Insert after Test 31:

```cpp
    // Test 32: async_send with multiple iovec
    {
        printf("Test 32: async_send multi-iovec... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();

        struct iovec iov[2];
        char buf1[] = "hello";
        char buf2[] = " world";
        iov[0].iov_base = buf1;
        iov[0].iov_len = 5;
        iov[1].iov_base = buf2;
        iov[1].iov_len = 6;  // " world" = 6 chars

        backend->async_send(fds[0], iov, 2, ActorId(1), static_cast<uint32_t>(OpType::Send));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result == 11 && "async_send multi-iovec should send 11 bytes");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Tests 26-32 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add multi-iovec async_send test

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 9: Add multi-iovec async_recv test

**Files:**
- Modify: `tests/net/test_event_loop.cpp`

- [ ] **Step 1: Add multi-iovec async_recv test**

Insert after Test 32:

```cpp
    // Test 33: async_recv with multiple iovec
    {
        printf("Test 33: async_recv multi-iovec... ");
        hpactor::net::EventLoop loop;
        std::optional<OpCompletion> captured;

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        loop.set_completion_callback([&captured](OpCompletion c) {
            captured = c;
        });

        auto* backend = loop.backend();

        // Write data to fd[1] - split across two buffers when read
        const char* msg = "helloworld";
        ssize_t written = ::write(fds[1], msg, 10);
        assert(written == 10);

        char buf1[5];
        char buf2[5];
        struct iovec iov[2];
        iov[0].iov_base = buf1;
        iov[0].iov_len = 5;
        iov[1].iov_base = buf2;
        iov[1].iov_len = 5;

        backend->async_recv(fds[0], iov, 2, ActorId(1), static_cast<uint32_t>(OpType::Recv));
        loop.process_completions();

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result == 10 && "async_recv multi-iovec should receive 10 bytes");
        assert(memcmp(buf1, "hello", 5) == 0 && "first buffer should be 'hello'");
        assert(memcmp(buf2, "world", 5) == 0 && "second buffer should be 'world'");

        ::close(fds[0]);
        ::close(fds[1]);
        printf("PASS\n");
    }
```

- [ ] **Step 2: Build and run**

Run: `ninja -C build && ./build/tests/net/test_event_loop`
Expected: Tests 26-33 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_event_loop.cpp
git commit -m "test(event_loop): add multi-iovec async_recv test

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Verification

After all tasks, run full test suite:

```bash
ninja -C build && ctest --output-on-failure
```

All 21 tests (13 original + 8 new) should pass.

---

## Spec Reference

`docs/superpowers/specs/2026-04-16-event-loop-async-io-tests-design.md`
