# Use epoll/kqueue for default event-loop backend fallback if io_using/libdispatch is not supported on older linux/macos kernel

# support default epoll/kqueue for older kernel 
 - in /Users/skg7on/Workspace/Projects/HPActor/src/net/event_loop.cpp, if the OS(linux or macos) kernel doesn't support new feature(io_using or libdispatch), fall back to use epool/queue.
 - provide explicit EventLoop::run() method, instead of start directly on object construction.
```c++
EventLoop::EventLoop() {
#if defined(__APPLE__)
    auto gcd_backend = std::make_unique<GcdBackend>();
    gcd_backend->set_loop(this);
    gcd_backend->start();
    backend_ = std::make_unique<BackendAdapter>(this, std::move(gcd_backend));
#elif defined(__linux__)
    auto iouring_backend = std::make_unique<IoUringBackend>();
    iouring_backend->start();
    backend_ = std::make_unique<BackendAdapter>(this, std::move(iouring_backend));
#else
    #error "Unsupported platform"
#endif
}
```