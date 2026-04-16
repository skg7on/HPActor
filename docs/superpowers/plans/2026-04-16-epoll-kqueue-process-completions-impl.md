# Epoll/Kqueue process_completions Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement process_completions() for EpollBackend and KqueueBackend using a pending completion queue pattern.

**Architecture:** Both backends use a pending_completions_ queue. async_* methods push completions to the queue instead of delivering immediately. process_completions() drains the queue and calls deliver_completion(). This integrates with EventLoop's completion processing loop.

**Tech Stack:** C++20, no exceptions, no RTTI. Linux (epoll) and macOS/BSD (kqueue).

---

## Files Modified

| File | Change |
|------|--------|
| `include/hpactor/net/epoll_backend.hpp` | Add pending_completions_, mutex_, helper methods |
| `src/net/epoll_backend.cpp` | Implement process_completions(); modify async_* to queue completions |
| `include/hpactor/net/kqueue_backend.hpp` | Add pending_completions_, mutex_, events_ array, last_num_events_ |
| `src/net/kqueue_backend.cpp` | Implement process_completions(); modify async_* to queue completions |

---

## Task 1: Add infrastructure to EpollBackend header

**Files:**
- Modify: `include/hpactor/net/epoll_backend.hpp`

- [ ] **Step 1: Read epoll_backend.hpp**

```bash
cat include/hpactor/net/epoll_backend.hpp
```

- [ ] **Step 2: Add pending completion queue and mutex**

Find the private section. Add:
```cpp
// Pending completions from async_* calls (for process_completions)
std::vector<OpCompletion> pending_completions_;
mutable std::mutex completions_mutex_;
```

- [ ] **Step 3: Add helper methods declaration**

In public section, add:
```cpp
// Process pending completions
void process_completions() override;
```

- [ ] **Step 4: Verify header compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/net/epoll_backend.hpp -o /dev/null 2>&1
```

---

## Task 2: Implement process_completions for EpollBackend

**Files:**
- Modify: `src/net/epoll_backend.cpp`

- [ ] **Step 1: Find and update async_send to queue completions**

Find `EpollBackend::async_send` (around line 300). Currently it does:
```cpp
deliver_completion(completion);
```

Change to push on queue:
```cpp
{
    std::lock_guard<std::mutex> lock(completions_mutex_);
    pending_completions_.push_back(completion);
}
```

- [ ] **Step 2: Update async_recv to queue completions**

Find `EpollBackend::async_recv` (around line 330). Same change.

- [ ] **Step 3: Update async_accept to queue completions**

Find `EpollBackend::async_accept` (around line 388). Same change.

- [ ] **Step 4: Update async_connect to queue completions**

Find `EpollBackend::async_connect` (around line 411). Same change.

- [ ] **Step 5: Update async_recvfrom to queue completions**

Find `EpollBackend::async_recvfrom` (around line 436). Same change.

- [ ] **Step 6: Update async_sendto to queue completions**

Find `EpollBackend::async_sendto` (around line 468). Same change.

- [ ] **Step 7: Implement process_completions()**

Find `EpollBackend::process_completions()` (around line 530) which currently is:
```cpp
void EpollBackend::process_completions() {
    // Completions are delivered immediately in epoll backend
    // This is a no-op for now
}
```

Replace with:
```cpp
void EpollBackend::process_completions() {
    std::vector<OpCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions = std::move(pending_completions_);
        pending_completions_.clear();
    }
    for (auto& completion : completions) {
        deliver_completion(completion);
    }
}
```

- [ ] **Step 8: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
```

---

## Task 3: Add infrastructure to KqueueBackend header

**Files:**
- Modify: `include/hpactor/net/kqueue_backend.hpp`

- [ ] **Step 1: Read kqueue_backend.hpp**

```bash
cat include/hpactor/net/kqueue_backend.hpp
```

- [ ] **Step 2: Add pending completion queue and mutex**

In private section, add:
```cpp
// Pending completions from async_* calls (for process_completions)
std::vector<OpCompletion> pending_completions_;
mutable std::mutex completions_mutex_;

// Storage for kevent array from last wait()
struct kevent events_[16];
int last_num_events_ = 0;
```

- [ ] **Step 3: Add helper methods declaration**

In public section, add:
```cpp
// Process pending completions
void process_completions() override;
```

- [ ] **Step 4: Verify header compiles**

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti -I include -c include/hpactor/net/kqueue_backend.hpp -o /dev/null 2>&1
```

---

## Task 4: Implement process_completions for KqueueBackend

**Files:**
- Modify: `src/net/kqueue_backend.cpp`

- [ ] **Step 1: Find and update async_send to queue completions**

Find `KqueueBackend::async_send` (around line 212). Same queue pattern as epoll.

- [ ] **Step 2: Update async_recv to queue completions**

Find `KqueueBackend::async_recv` (around line 239). Same change.

- [ ] **Step 3: Update async_accept to queue completions**

Find `KqueueBackend::async_accept` (around line 288). Same change.

- [ ] **Step 4: Update async_connect to queue completions**

Find `KqueueBackend::async_connect` (around line 311). Same change.

- [ ] **Step 5: Update async_recvfrom to queue completions**

Find `KqueueBackend::async_recvfrom` (around line 335). Same change.

- [ ] **Step 6: Update async_sendto to queue completions**

Find `KqueueBackend::async_sendto` (around line 367). Same change.

- [ ] **Step 7: Update async_send_fixed and async_recv_fixed to queue completions**

Find these methods (around lines 254 and 271). Same change.

- [ ] **Step 8: Implement process_completions()**

Find `KqueueBackend::process_completions()` (around line 455). Replace with:
```cpp
void KqueueBackend::process_completions() {
    // Process pending completions from async_* calls
    std::vector<OpCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions = std::move(pending_completions_);
        pending_completions_.clear();
    }
    for (auto& completion : completions) {
        deliver_completion(completion);
    }

    // Process socket events from last wait()
    // Note: Events are stored in events_[] by wait() and cleared here
    for (int i = 0; i < last_num_events_; ++i) {
        if (events_[i].filter == EVFILT_TIMER) {
            continue;  // Timer events are handled in wait()
        }
        int fd = static_cast<int>(events_[i].ident);
        // Find actor associated with this fd - for now, use actor from completion
        // The actual actor routing happens via user_data in async_* calls
    }
    last_num_events_ = 0;
}
```

Wait() needs to store events. Find wait() (around line 395) and add after getting num_events:
```cpp
// Store events for process_completions to handle
if (num_events > 0 && num_events <= 16) {
    for (int i = 0; i < num_events; ++i) {
        events_[i] = events[i];
    }
    last_num_events_ = num_events;
}
```

- [ ] **Step 9: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
```

---

## Task 5: Build, test, and commit

- [ ] **Step 1: Full build**

```bash
ninja -C build 2>&1
```

- [ ] **Step 2: Run tests**

```bash
ctest --output-on-failure
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/epoll_backend.hpp src/net/epoll_backend.cpp include/hpactor/net/kqueue_backend.hpp src/net/kqueue_backend.cpp
git commit -m "$(cat <<'EOF'
feat(net): implement process_completions for epoll and kqueue backends

Add pending_completions_ queue to both backends. async_* methods
now queue completions instead of delivering immediately.
process_completions() drains the queue and calls deliver_completion().
Integrates with EventLoop's completion processing.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Verification Checklist

- [ ] Task 1: EpollBackend header has pending_completions_ and mutex
- [ ] Task 2: EpollBackend async_* methods queue completions
- [ ] Task 2: EpollBackend process_completions() drains queue
- [ ] Task 3: KqueueBackend header has pending_completions_, mutex, events_
- [ ] Task 4: KqueueBackend async_* methods queue completions
- [ ] Task 4: KqueueBackend wait() stores events for process_completions
- [ ] Task 4: KqueueBackend process_completions() drains queue
- [ ] Task 5: Build succeeds
- [ ] Task 5: All tests pass
- [ ] Task 5: Changes committed
