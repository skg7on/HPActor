# Event Loop Async I/O Tests Design

## Goal

Add tests for `async_send`, `async_recv`, `async_sendto`, and `async_recvfrom` operations in the EventLoop layer using kqueue backend on macOS. Tests verify correct completion delivery and data integrity.

## Background

The existing `tests/net/test_event_loop.cpp` covers timer and FD registration functionality. The async I/O operations (`async_send`, `async_recv`, etc.) are implemented in kqueue_backend but lack direct tests. These operations are synchronous wrappers that immediately perform I/O and queue `OpCompletion` results.

## Architecture

### Current Flow

```
async_send(fd, bufs, count, actor, op_type)
  → ::send(fd, data, flags)  [synchronous]
  → OpCompletion{.result=bytes_sent}
  → deliver_completion() → EventLoop::enqueue_completion()
  → actor_system_->enqueue_completion()  OR  (if no actor_system) dropped
```

### Test Challenge

Without an `ActorSystem`, completions are dropped. Tests need a way to verify completions.

### Solution: Optional Completion Callback

Add an optional `completion_callback` to `EventLoop` (disabled by default, enabled for tests):

```cpp
using completion_callback = std::function<void(OpCompletion)>;
void set_completion_callback(completion_callback cb);
```

When set, `enqueue_completion()` invokes the callback instead of dropping.

## Test Cases

### 1. async_send - Basic Send

**Setup:** socketpair(AF_UNIX, SOCK_STREAM)
**Action:**
1. Call `backend()->async_send(fd[0], iovec{"hello"}, 1, actor, OpType::Send)`
2. Call `loop.process_completions()`
**Verify:** Completion callback received with result=5, fd=fd[0], type=OpType::Send

### 2. async_recv - Basic Receive

**Setup:** socketpair(AF_UNIX, SOCK_STREAM)
**Action:**
1. Write "hello" to fd[1]
2. Call `backend()->async_recv(fd[0], buffer, 1, actor, OpType::Recv)`
3. Call `loop.process_completions()`
**Verify:** Completion callback received with result=5, fd=fd[0], type=OpType::Recv

### 3. async_sendto - Send To Address

**Setup:** socketpair(AF_UNIX, SOCK_STREAM) - address ignored on connected sockets
**Action:**
1. Call `backend()->async_sendto(fd[0], iovec{"test"}, 1, addr, addrlen, actor, OpType::SendTo)`
2. Call `loop.process_completions()`
**Verify:** Completion callback received with result=4, type=OpType::SendTo

### 4. async_recvfrom - Receive From

**Setup:** socketpair(AF_UNIX, SOCK_STREAM)
**Action:**
1. Write "world" to fd[1]
2. Call `backend()->async_recvfrom(fd[0], buffer, 1, actor, OpType::RecvFrom)`
3. Call `loop.process_completions()`
**Verify:** Completion callback received with result=5, type=OpType::RecvFrom

### 5. async_send - Error on Closed FD

**Setup:** socketpair + close one fd
**Action:**
1. Close fd[0]
2. Call `backend()->async_send(fd[0], ...)`

**Verify:** Completion callback received with result=EBADF (or negative errno)

### 6. async_recv - Empty Buffer

**Setup:** socketpair(AF_UNIX, SOCK_STREAM)
**Action:**
1. Call `backend()->async_recv(fd[0], empty_bufs, 0, actor, OpType::Recv)`
2. Call `loop.process_completions()`
**Verify:** Completion callback received (likely result=0 for zero-length read)

### 7. async_send - Multiple I/O Vectors

**Setup:** socketpair
**Action:**
1. Call `async_send` with iovec[2] = {"hello", " world"} (total 11 bytes)
2. `process_completions()`
**Verify:** result=11

### 8. async_recv - Multiple I/O Vectors

**Setup:** socketpair
**Action:**
1. Write "helloworld" to fd[1]
2. Call `async_recv` with iovec[2] = {buf1(5), buf2(5)}
3. `process_completions()`
**Verify:** Both buffers filled, result=10

## File Structure

```
tests/net/
  └── test_event_loop.cpp    # Append async I/O tests to existing file
include/hpactor/net/
  └── event_loop.hpp          # Add set_completion_callback()
src/net/
  └── event_loop.cpp          # Implement completion callback
```

## Implementation Steps

1. **Add `completion_callback` to EventLoop**
   - Add `std::optional<completion_callback> completion_callback_`
   - Add `set_completion_callback()` method
   - Modify `enqueue_completion()` to invoke callback if set

2. **Add async_send test**
   - Create socketpair
   - Set completion callback
   - Call async_send, process_completions, verify

3. **Add async_recv test**
   - Write data to socket, async_recv, verify

4. **Add async_sendto/async_recvfrom tests**
   - Same pattern with UDP-style operations

5. **Add error handling tests**
   - Closed fd, verify errno

## Notes

- Tests use `KqueueBackend` directly via `EventLoop::backend()` on macOS
- Socketpair provides reliable connected socket testing without server/client complexity
- kqueue_backend's async_* operations are synchronous wrappers - they complete immediately
- Completion callback signature matches `std::function<void(OpCompletion)>`
