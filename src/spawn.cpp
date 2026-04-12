#include <hpactor/spawn.hpp>
#include <hpactor/ref/actor_proxy.hpp>

namespace hpactor {

AsyncActor::AsyncActor()
    : mutex_(std::make_unique<std::mutex>())
    , cv_(std::make_unique<std::condition_variable>()) {}

AsyncActor::AsyncActor(NodeId node_id, std::chrono::milliseconds timeout)
    : node_id_(node_id)
    , timeout_(timeout)
    , mutex_(std::make_unique<std::mutex>())
    , cv_(std::make_unique<std::condition_variable>()) {}

AsyncActor::AsyncActor(AsyncActor&& other) noexcept
    : node_id_(other.node_id_)
    , timeout_(other.timeout_)
    , mutex_(std::move(other.mutex_))
    , cv_(std::move(other.cv_))
    , ready_(other.ready_)
    , cancelled_(other.cancelled_)
    , response_(other.response_) {}

AsyncActor& AsyncActor::operator=(AsyncActor&& other) noexcept {
    if (this != &other) {
        node_id_ = other.node_id_;
        timeout_ = other.timeout_;
        mutex_ = std::move(other.mutex_);
        cv_ = std::move(other.cv_);
        ready_ = other.ready_;
        cancelled_ = other.cancelled_;
        response_ = other.response_;
    }
    return *this;
}

result<ActorRef> AsyncActor::get() {
    std::unique_lock<std::mutex> lock(*mutex_);
    if (cancelled_) {
        return result<ActorRef>::make(error(errors::unknown, "spawn cancelled"));
    }

    bool timed_out = !cv_->wait_for(lock, timeout_, [this] { return ready_; });
    if (timed_out) {
        return result<ActorRef>::make(error(errors::timeout, "spawn request timed out"));
    }

    if (response_.error_code != spawn_errors::success) {
        return result<ActorRef>::make(error(response_.error_code, "spawn failed"));
    }

    // Create ActorProxy for the remote actor using stack allocation
    ActorProxy proxy(response_.actor_addr, nullptr);
    ActorRef ref(std::move(proxy));
    return result<ActorRef>::make(std::move(ref));
}

bool AsyncActor::ready() const {
    std::lock_guard<std::mutex> lock(*mutex_);
    return ready_ || cancelled_;
}

void AsyncActor::cancel() {
    std::lock_guard<std::mutex> lock(*mutex_);
    cancelled_ = true;
    ready_ = true;
    cv_->notify_all();
}

void AsyncActor::set_response(SpawnResponse response) {
    std::lock_guard<std::mutex> lock(*mutex_);
    response_ = response;
    ready_ = true;
    cv_->notify_all();
}

} // namespace hpactor