#include <hpactor/net/transport.hpp>

namespace hpactor {

namespace net {

Connection::Connection(NodeId remote_node)
    : remote_node_(remote_node) {}

Connection::~Connection() = default;

void Connection::set_message_handler(message_handler handler) {
    message_handler_ = std::move(handler);
}

void Connection::handle_read(const bytes& data) {
    read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());

    // Simple framing: look for message boundary (newline for now)
    // TODO: implement proper length-prefixed framing
    while (!read_buffer_.empty()) {
        auto it = std::find(read_buffer_.begin(), read_buffer_.end(), '\n');
        if (it == read_buffer_.end()) {
            break;
        }

        bytes message(read_buffer_.begin(), it);
        read_buffer_.erase(read_buffer_.begin(), it + 1);

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

} // namespace net
} // namespace hpactor
