# Epoll/Kqueue process_completions Implementation Design

**Date:** 2026-04-16
**Status:** Draft
**Goal:** Complete process_completions() implementation for EpollBackend and KqueueBackend to integrate with the scheduling subsystem.

## Overview

Both `EpollBackend` and `KqueueBackend` have `process_completions()` as a no-op. The async I/O methods already deliver completions synchronously, but the backends don't properly integrate with the EventLoop's `process_completions()` call chain.

After this design:
- `wait()` will store socket fd events for later processing
- `process_completions()` will drain those events and deliver OpCompletions
- The EventLoop's network thread can process completions between wait() calls

## Architecture

**Pattern (per backends' existing design):**
- `async_*` methods do synchronous I/O and call `deliver_completion()` immediately
- `wait()` blocks waiting for timer events (and socket events for kqueue)
- `process_completions()` is called by EventLoop after wait() returns

**The fix:**
- For epoll: `wait()` already ignores socket events (they're edge-triggered). Socket events are handled synchronously in `async_*`. The completion delivery is already correct.
- For kqueue: `wait()` returns event count, but events array goes unused. Need to process them in `process_completions()`.
- Add a `pending_completions_` queue to store completions from async_* methods
- `process_completions()` drains the queue and delivers to the loop

## Files Modified

| File | Change |
|------|--------|
| `src/net/epoll_backend.cpp` | Add pending_completions_ queue; modify async_* to queue instead of immediate delivery; implement process_completions() |
| `src/net/kqueue_backend.cpp` | Add pending_completions_ queue; modify async_* to queue; implement process_completions() to process kevent events |

## Implementation Details

### EpollBackend

1. **Add pending completion queue** to store completions between async_* and process_completions()
2. **Modify async_send/async_recv** to queue completion instead of immediate delivery:
   - Attempt the I/O
   - Create OpCompletion
   - Push to pending_completions_ queue (not deliver_completion)
3. **Implement process_completions()** to drain pending_completions_:
```cpp
void EpollBackend::process_completions() {
    std::vector<OpCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions = std::move(pending_completions_);
        pending_completions_.clear();
    }
    for (auto& completion : completions) {
        deliver_completion(completion);
    }
}
```

### KqueueBackend

1. **Add pending completion queue**
2. **Modify async_* to queue completions**
3. **Implement process_completions()** to process kevent socket events:
```cpp
void KqueueBackend::process_completions() {
    // Process pending completions from async_* calls
    std::vector<OpCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions = std::move(pending_completions_);
        pending_completions_.clear();
    }
    for (auto& completion : completions) {
        deliver_completion(completion);
    }

    // Process socket events from last wait()
    for (int i = 0; i < last_num_events_; ++i) {
        int fd = static_cast<int>(events_[i].ident);
        auto it = fd_actors_.find(fd);
        if (it != fd_actors_.end()) {
            OpCompletion completion{
                .actor = it->second,
                .type = events_[i].filter == EVFILT_READ ? OpType::Recv : OpType::Send,
                .fd = fd,
                .result = 0,
                .user_data = 0,
            };
            deliver_completion(completion);
        }
    }
}
```

## Header Changes

Need to add to `epoll_backend.hpp` and `kqueue_backend.hpp`:
- `std::vector<OpCompletion> pending_completions_`
- `std::mutex mutex_`
- Storage for events array and last_num_events in kqueue

## Testing

1. Build: `ninja -C build`
2. Run tests: `ctest --output-on-failure`
3. Manual test: spawn actor, initiate network I/O, verify completion delivered
