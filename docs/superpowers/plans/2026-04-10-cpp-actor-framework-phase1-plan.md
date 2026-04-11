# C++ Actor Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the foundation of a C++20 Actor Framework - starting with the MPSC Mailbox. This phase establishes the mailbox interface and a mutex-based implementation that will later be upgraded to lock-free.

**Architecture:** Phase 1 focuses on the Mailbox component. The design uses swap-in interfaces allowing lock-free replacement without changing actor code. Start with mutex for safety, earn lock-free through testing.

**Tech Stack:** C++20, CMake, ThreadSanitizer (TSan)

**Project Scope:** This is a multi-phase implementation. This plan covers Phase 1 (Mailbox). Subsequent plans will cover Message Bus, Scheduler, and Coroutines.

---

## File Structure

```
HPActor/
├── include/hpactor/
│   ├── platform.hpp          # Platform detection (Linux/macOS)
│   ├── message.hpp          # Message<T> wrapper
│   ├── mailbox.hpp          # IMailbox<T> interface
│   └── mutex_mailbox.hpp   # MutexMailbox<T> implementation
├── src/
│   └── (header-only for now)
├── tests/
│   ├── test_mailbox.cpp    # Mailbox unit tests
│   └── CMakeLists.txt
├── CMakeLists.txt
└── docs/superpowers/plans/
    └── 2026-04-10-cpp-actor-framework-phase1-plan.md
```

---

## Phase 1: Mailbox Implementation Plan

### Phase 1 Design Decision: ArenaAllocator Deferral

**Design Spec Note:** The spec requires ArenaAllocator as "1. ArenaAllocator: Foundation, no dependencies". However, to maintain TDD bite-sized steps, Phase 1 uses `std::queue` with the understanding that:

1. **This is acceptable for mutex-based Phase 1** - We're testing the interface, not the allocator
2. **Swap-in interface** - The IMailbox interface allows swapping in arena-backed implementation later
3. **Phase 2+ will add ArenaAllocator** - When we proceed to Message Bus, we'll add proper arena allocation

This matches the original roadmap: "Do not let Claude write a lock-free queue on day one... build a highly optimized queue using std::mutex or a lightweight std::atomic_flag spinlock first."

---

### Task 1: Project Setup

**Files:**
- Create: `CMakeLists.txt`
- Create: `include/hpactor/platform.hpp`

- [ ] **Step 1: Create CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
project(HPActor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce TSan in CI builds
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# Header-only library
add_library(hpactor INTERFACE)
target_include_directories(hpactor INTERFACE include)

# Tests
enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 2: Create platform.hpp**

```cpp
#pragma once

#include <cstddef>

#ifdef __linux__
    #define HPACTOR_PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define HPACTOR_PLATFORM_MACOS 1
#else
    #define HPACTOR_PLATFORM_UNKNOWN 1
#endif

namespace hpactor {
using byte_t = unsigned char;

inline constexpr size_t default_mailbox_capacity = 1024;
}
```

- [ ] **Step 3: Create tests/CMakeLists.txt**

```cmake
# Tests CMakeLists.txt

# test_message
add_executable(test_message tests/test_message.cpp)
target_link_libraries(test_message hpactor)

# test_message_advanced
add_executable(test_message_advanced tests/test_message_advanced.cpp)
target_link_libraries(test_message_advanced hpactor)

# test_mailbox_interface
add_executable(test_mailbox_interface tests/test_mailbox_interface.cpp)
target_link_libraries(test_mailbox_interface hpactor)

# test_mutex_mailbox
add_executable(test_mutex_mailbox tests/test_mutex_mailbox.cpp)
target_link_libraries(test_mutex_mailbox hpactor)

# test_mailbox_stress
add_executable(test_mailbox_stress tests/test_mailbox_stress.cpp)
target_link_libraries(test_mailbox_stress hpactor)

# test_mailbox_factory
add_executable(test_mailbox_factory tests/test_mailbox_factory.cpp)
target_link_libraries(test_mailbox_factory hpactor)

# Register all tests with CTest
add_test(NAME test_message COMMAND test_message)
add_test(NAME test_message_advanced COMMAND test_message_advanced)
add_test(NAME test_mailbox_interface COMMAND test_mailbox_interface)
add_test(NAME test_mutex_mailbox COMMAND test_mutex_mailbox)
add_test(NAME test_mailbox_stress COMMAND test_mailbox_stress)
add_test(NAME test_mailbox_factory COMMAND test_mailbox_factory)
```

- [ ] **Step 3: Verify build**

Run: `mkdir -p build && cd build && cmake .. && cmake --build .`
Expected: SUCCESS (no output means success)

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt include/hpactor/platform.hpp
git commit -m "feat: add project setup with CMake and platform detection"
```

---

### Task 2: Message Wrapper

**Files:**
- Create: `include/hpactor/message.hpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_message.cpp
#include <hpactor/message.hpp>
#include <string>
#include <cassert>

struct TestPayload {
    int value;
    std::string data;
};

int main() {
    // Test default construction
    hpactor::Message<TestPayload> msg;
    // Test with payload
    hpactor::Message<TestPayload> msg2{TestPayload{42, "hello"}};
    assert(msg2.payload().value == 42);
    assert(msg2.payload().data == "hello");
    // Test move semantics
    TestPayload p{100, "moved"};
    hpactor::Message<TestPayload> msg3{std::move(p)};
    assert(msg3.payload().value == 100);
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . && ./tests/test_message`
Expected: FAIL - file does not exist

- [ ] **Step 3: Write minimal implementation**

```cpp
#pragma once
#include <utility>
#include <type_traits>

namespace hpactor {

// Error codes for hot-path operations (no exceptions)
enum class MailboxError {
    Success,
    Full,       // Mailbox at capacity
    Empty,      // Mailbox empty on pop
    Invalid     // Invalid state
};

template<typename T>
class Message {
public:
    Message() = default;

    // Perfect forwarding constructor - handles both copy and move
    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U, T>>>
    explicit Message(U&& payload) : payload_(std::forward<U>(payload)) {}

    T& payload() noexcept { return payload_; }
    const T& payload() const noexcept { return payload_; }

    // Allow explicit access to underlying payload move
    T&& move_payload() noexcept { return std::move(payload_); }

private:
    T payload_;
};

}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . && ./tests/test_message`
Expected: PASS

- [ ] **Step 5: Add more move semantics tests**

```cpp
// tests/test_message_advanced.cpp
#include <hpactor/message.hpp>
#include <string>
#include <cassert>

struct MoveOnly {
    int value;
    std::string data;
    MoveOnly() = default;
    MoveOnly(int v, std::string d) : value(v), data(std::move(d)) {}
    MoveOnly(MoveOnly&& other) noexcept : value(other.value), data(std::move(other.data)) {}
    MoveOnly& operator=(MoveOnly&& other) noexcept {
        value = other.value;
        data = std::move(other.data);
        return *this;
    }
    // Delete copy
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
};

int main() {
    // Test move-only type
    MoveOnly m{42, "test"};
    hpactor::Message<MoveOnly> msg{std::move(m)};
    assert(msg.payload().value == 42);
    // Verify original moved-from state
    assert(m.value == 42);  // int copied, string moved-from
    
    // Test const lvalue
    const MoveOnly cm{100, "const"};
    hpactor::Message<MoveOnly> msg2{cm};
    assert(msg2.payload().value == 100);
    
    return 0;
}
```

- [ ] **Step 6: Run advanced tests**

Run: `cd build && cmake --build . && ./tests/test_message_advanced`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/message.hpp tests/test_message.cpp
git commit -m "feat: add Message<T> wrapper with move semantics"
```

---

### Task 3: IMailbox Interface

**Files:**
- Create: `include/hpactor/mailbox.hpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_mailbox_interface.cpp
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>
#include <string>
#include <cassert>
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . && ./tests/test_mailbox_interface`
Expected: FAIL - mailbox.hpp doesn't exist

- [ ] **Step 3: Write minimal implementation**

```cpp
#pragma once
#include <cstddef>
#include <hpactor/message.hpp>

namespace hpactor {

template<typename T>
class IMailbox {
public:
    virtual ~IMailbox() = default;

    // Hot path - marked noexcept for real-time guarantees
    virtual void push(Message<T>&& msg) noexcept = 0;
    virtual bool try_pop(Message<T>& out) noexcept = 0;

    // Blocking pop - may block, used for actor message processing loop
    virtual bool pop(Message<T>& out) = 0;

    virtual size_t size() const = 0;
    virtual bool empty() const = 0;
};

}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . && ./tests/test_mailbox_interface`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox.hpp tests/test_mailbox_interface.cpp
git commit -m "feat: add IMailbox<T> interface"
```

---

### Task 4: MutexMailbox Implementation

**Files:**
- Create: `include/hpactor/mutex_mailbox.hpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_mutex_mailbox.cpp
#include <hpactor/mutex_mailbox.hpp>
#include <hpactor/message.hpp>
#include <string>
#include <cassert>
#include <thread>
#include <vector>

struct PingMsg {
    int value;
};

int main() {
    hpactor::MutexMailbox<PingMsg> mailbox;
    
    // Test push/pop
    mailbox.push(Message<PingMsg>{PingMsg{42}});
    assert(mailbox.size() == 1);
    
    Message<PingMsg> msg;
    bool popped = mailbox.pop(msg);
    assert(popped);
    assert(msg.payload().value == 42);
    assert(mailbox.empty());
    
    // Test try_pop on empty
    bool tried = mailbox.try_pop(msg);
    assert(!tried);  // Should return false
    
    // Test thread safety - push from multiple threads
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&mailbox, i]() {
            for (int j = 0; j < 100; ++j) {
                mailbox.push(Message<PingMsg>{PingMsg{i * 100 + j}});
            }
        });
    }
    for (auto& t : threads) t.join();
    
    assert(mailbox.size() == 1000);  // All messages delivered
    
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . && ./tests/test_mutex_mailbox`
Expected: FAIL - mutex_mailbox.hpp doesn't exist

- [ ] **Step 3: Write minimal implementation**

```cpp
#pragma once
#include <queue>
#include <mutex>
#include <memory>
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>

namespace hpactor {

template<typename T>
class MutexMailbox : public IMailbox<T> {
public:
    MutexMailbox() = default;

    // Hot path - marked noexcept for real-time guarantees
    void push(Message<T>&& msg) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(msg));
    }

    bool pop(Message<T>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Hot path - marked noexcept for real-time guarantees
    bool try_pop(Message<T>& out) noexcept override {
        return pop(out);
    }

    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::queue<Message<T>> queue_;
};

}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . && ./tests/test_mutex_mailbox`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mutex_mailbox.hpp tests/test_mutex_mailbox.cpp
git commit -m "feat: add MutexMailbox<T> implementation"
```

---

### Task 5: Stress Test (Prerequisite for Lock-Free)

**Files:**
- Create: `tests/test_mailbox_stress.cpp`

- [ ] **Step 1: Write the stress test**

```cpp
// tests/test_mailbox_stress.cpp
#include <hpactor/mutex_mailbox.hpp>
#include <hpactor/message.hpp>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>

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
                mailbox.push(Message<StressMsg>{StressMsg{i * 1000 + j}});
                count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();
    
    // Drain all messages
    int popped = 0;
    Message<StressMsg> msg;
    while (mailbox.try_pop(msg)) {
        popped++;
    }
    
    assert(popped == num_threads * msgs_per_thread);
    printf("Stress test passed: %d messages from %d threads\n", popped, num_threads);
    
    return 0;
}
```

- [ ] **Step 2: Run stress test**

Run: `cd build && cmake --build . && ./tests/test_mailbox_stress`
Expected: PASS (all 1,000,000 messages processed)

- [ ] **Step 3: Run with ThreadSanitizer**

Run: `cmake -DENABLE_TSAN=ON .. && cmake --build . && ./tests/test_mailbox_stress`
Expected: PASS with no TSan warnings

- [ ] **Step 4: Commit**

```bash
git add tests/test_mailbox_stress.cpp
git commit -m "test: add mailbox stress test (1M messages, 100 threads)"
```

---

### Task 6: Make Mailbox Factory (Swap-In Interface)

**Files:**
- Modify: `include/hpactor/mailbox.hpp`

- [ ] **Step 1: Add mailbox factory to interface**

```cpp
// Add to include/hpactor/mailbox.hpp

namespace hpactor {

// Default implementation selector
enum class MailboxType {
    Mutex,       // Thread-safe, slower
    LockFree,   // TODO: Implementation TBD after stress tests pass
};

// Factory function
template<typename T, MailboxType Type = MailboxType::Mutex>
std::unique_ptr<IMailbox<T>> create_mailbox() {
    if constexpr (Type == MailboxType::Mutex) {
        return std::make_unique<MutexMailbox<T>>();
    }
    // LockFree would be added here later
}

}
```

- [ ] **Step 2: Update test to use factory**

```cpp
// tests/test_mailbox_factory.cpp
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>
#include <cassert>

struct SimpleMsg {};

int main() {
    auto mailbox = hpactor::create_mailbox<SimpleMsg>();
    mailbox->push(Message<SimpleMsg>{});
    assert(mailbox->size() == 1);
    return 0;
}
```

- [ ] **Step 3: Run test**

Run: `cd build && cmake --build . && ./tests/test_mailbox_factory`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox.hpp tests/test_mailbox_factory.cpp
git commit -m "feat: add mailbox factory with swap-in interface"
```

---

## Phase 1 Complete

**Summary:** Phase 1 establishes the Mailbox foundation:
- ✅ Message<T> wrapper with move semantics
- ✅ IMailbox<T> interface
- ✅ MutexMailbox<T> implementation
- ✅ Factory with swap-in interface
- ✅ Stress test passes (1M messages, 100 threads)
- ✅ ThreadSanitizer-clean

**Next Phase:** Message Bus (Phase 2) - Type-safe messages using std::variant

---

## Pending Decisions for Future Phases

1. Lock-free algorithm selection (proceed after Phase 1 stress test earns the complexity)
2. Exact buffer sizes for arena (message count/size limits)
3. Thread count auto-scaling thresholds
4. Stackful vs stackless coroutines

---

**Status:** Implementation ready to begin. Proceed with Task 1: Project Setup.