// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/net/event_loop.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// Acceptor - server socket listener for incoming connections
// -----------------------------------------------------------------------------
class Acceptor {
public:
    using accept_handler = std::function<void(int client_fd, NodeId /*remote_node_hint*/)>;

    Acceptor(EventLoop* loop);
    ~Acceptor();

    // Non-copyable
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    // Start listening on the specified port
    // Returns true on success, false on failure
    bool listen(uint16_t port, uint16_t port_range = 0);

    // Stop listening and close the socket
    void close();

    // Set handler for accepted connections
    void set_accept_handler(accept_handler handler);

    // Check if listening
    bool is_listening() const { return listening_fd_ >= 0; }

    // Get the bound port
    uint16_t port() const { return bound_port_; }

private:
    void handle_read();

    EventLoop* loop_;
    int listening_fd_ = -1;
    uint16_t bound_port_ = 0;
    accept_handler accept_handler_;
};

} // namespace net
} // namespace hpactor
