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

Connection::Connection(CommunicationEndpoint remote_endpoint)
    : remote_endpoint_(remote_endpoint) {}

Connection::~Connection() = default;

void Connection::set_message_handler(message_handler handler) {
    message_handler_ = std::move(handler);
}

void Connection::handle_read(const bytes& data) {
    read_buffer_.append(data.data(), data.size());

    // Simple framing: look for message boundary (newline for now)
    // TODO: implement proper length-prefixed framing
    while (!read_buffer_.empty()) {
        auto* begin = read_buffer_.data();
        auto* end = begin + read_buffer_.size();
        auto it = std::find(begin, end, '\n');
        if (it == end) {
            break;
        }

        bytes message(begin, it);
        size_t consumed = static_cast<size_t>(it - begin) + 1; // +1 for '\n'
        read_buffer_.consume(consumed);

        if (message_handler_ && !message.empty()) {
            on_message(message);
        }
    }
}

void Connection::set_state(ConnectionState new_state) {
    state_ = new_state;
}

void Connection::on_message(const bytes& data) {
    if (message_handler_) {
        message_handler_(data);
    }
}

void Connection::handle_send_completion(int /*result*/) {
    // Default no-op implementation. Derived classes override to handle
    // send completions from the EventLoop.
}

} // namespace net
} // namespace hpactor
