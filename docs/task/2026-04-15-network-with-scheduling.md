# When actor system start network event loop in background thread, process events until stopped

 - in src/actor/actor_system.cpp, ActorSystem::ActorSystem() create a background thread waiting for network events (message send to actor)
 - if the `wait` return with network event happened, they should process events until stopped
 - finish the implementation that process event with scheduling sub-system
 
```c++
// Start network event loop in background thread
network_thread_ = std::thread([this]() {
    while (network_loop_->wait(100) >= 0) {
        // Process events until stopped
        }
    });
```

## Epoll backend