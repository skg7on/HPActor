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

#include <hpactor/net/transport.hpp>

namespace hpactor {

namespace net {

Connection::Connection(int fd, EndPoint local_endpoint,
                       EndPoint remote_endpoint, EventLoop* loop)
    : fd_(fd), local_endpoint_(local_endpoint),
      remote_endpoint_(remote_endpoint), loop_(loop) {}

Connection::~Connection() = default;

void Connection::set_state(ConnectionState new_state) {
    state_ = new_state;
}

void Connection::handle_send_completion(int /*result*/) {
    // Default no-op. Derived classes override to handle
    // send completions from the EventLoop.
}

} // namespace net
} // namespace hpactor
