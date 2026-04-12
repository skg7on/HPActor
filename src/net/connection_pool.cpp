#include <hpactor/net/connection_pool.hpp>

namespace hpactor {

namespace net {

ConnectionPool::ConnectionPool(NodeId remote_node_id,
                              const PoolConfig& config,
                              TlsContext* tls_context,
                              EventLoop* loop)
    : Connection(remote_node_id),
      remote_node_id_(remote_node_id),
      config_(config),
      tls_context_(tls_context),
      loop_(loop) {}

ConnectionPool::~ConnectionPool() {
    abort();
}

TlsConnectionPtr ConnectionPool::get_connection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_connections_.empty()) {
        return nullptr;
    }
    auto index = next_index_.fetch_add(1) % active_connections_.size();
    return active_connections_[index];
}

void ConnectionPool::send(const ActorAddress& target, const bytes& encoded) {
    if (shutting_down_.load()) {
        return;
    }

    TlsConnectionPtr conn = get_connection();
    if (conn) {
        conn->send(encoded);
        return;
    }

    // No connection available, queue pending
    if (!add_pending(target, encoded)) {
        return;  // Queue full
    }

    // Create connection
    create_connection();
}

void ConnectionPool::send(const bytes& data) {
    // Create a minimal actor address using the remote node ID
    ActorAddress target;
    target.node_id = remote_node();
    send(target, data);
}

void ConnectionPool::close() {
    abort();
}

bool ConnectionPool::is_connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_connections_.empty();
}

PoolStats ConnectionPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PoolStats s;
    s.active_connections = active_connections_.size();
    s.pending_messages = pending_messages_.size();
    s.reconnect_attempts = reconnect_attempts_.load();
    s.is_connected = !active_connections_.empty();
    return s;
}

size_t ConnectionPool::drain() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t unsent = pending_messages_.size();
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
    pending_messages_.clear();
    return unsent;
}

void ConnectionPool::abort() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
    pending_messages_.clear();
}

void ConnectionPool::create_connection() {
    if (connecting_.load()) {
        return;
    }
    if (!connecting_.exchange(true)) {
        auto conn = TlsConnection::create_client(
            remote_node_id_, tls_context_, loop_);

        conn->set_ready_handler([this](TlsConnectionPtr c) {
            on_connection_ready(c);
        });
        conn->set_error_handler([this](TlsConnectionPtr c, const error& e) {
            on_connection_error(c, e);
        });
        conn->set_frame_handler([this](const bytes& data) {
            on_frame_received(data);
        });

        conn->start_client_handshake();
    }
}

void ConnectionPool::on_connection_ready(TlsConnectionPtr conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.push_back(conn);
    }
    connecting_.store(false);
    flush_pending();
}

void ConnectionPool::on_connection_error(TlsConnectionPtr conn, const error& err) {
    (void)err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.erase(
            std::remove(active_connections_.begin(), active_connections_.end(), conn),
            active_connections_.end());
    }
    schedule_reconnect();
}

void ConnectionPool::on_frame_received(const bytes& frame_data) {
    // Deliver to callback or ActorSystem
    (void)frame_data;
}

void ConnectionPool::schedule_reconnect() {
    if (shutting_down_.load()) {
        return;
    }
    if (reconnect_attempts_.load() >= config_.max_attempts) {
        return;  // Exhausted retries
    }
    if (reconnect_scheduled_.load()) {
        return;
    }
    reconnect_scheduled_.store(true);

    auto backoff = config_.initial_backoff;
    auto attempts = reconnect_attempts_.load();
    for (size_t i = 0; i < attempts; ++i) {
        backoff = backoff * 2;
        if (backoff > config_.max_backoff) {
            backoff = config_.max_backoff;
        }
    }

    reconnect_attempts_.fetch_add(1);
    loop_->run_after([this]() {
        reconnect_scheduled_.store(false);
        connecting_.store(false);
        if (!shutting_down_.load()) {
            create_connection();
        }
    }, static_cast<int>(backoff.count()));
}

void ConnectionPool::flush_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_messages_.empty() && !active_connections_.empty()) {
        auto& msg = pending_messages_.front();
        auto conn = get_connection();
        if (conn) {
            conn->send(msg.data);
            pending_messages_.pop_front();
        } else {
            break;
        }
    }
}

bool ConnectionPool::add_pending(const ActorAddress& target, const bytes& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_messages_.size() >= config_.max_pending) {
        return false;
    }
    pending_messages_.push_back({target, data, std::chrono::steady_clock::now()});
    return true;
}

} // namespace net
} // namespace hpactor