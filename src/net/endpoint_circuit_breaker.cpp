#include <hpactor/net/endpoint_circuit_breaker.hpp>

namespace hpactor::net {

EndpointCircuitBreaker::EndpointCircuitBreaker(const EndpointCircuitBreakerConfig& config)
    : config_(config) {}

void EndpointCircuitBreaker::record_failure() {
    State current = state_.load(std::memory_order_acquire);
    if (current == State::Closed) {
        size_t count = failure_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count >= config_.failure_threshold) {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_at_ = std::chrono::steady_clock::now();
            state_.store(State::Open, std::memory_order_release);
        }
    } else if (current == State::HalfOpen) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_count_.store(0, std::memory_order_release);
        half_open_probes_.store(0, std::memory_order_release);
        state_.store(State::Open, std::memory_order_release);
    }
}

void EndpointCircuitBreaker::record_success() {
    State current = state_.load(std::memory_order_acquire);
    if (current == State::Closed) {
        failure_count_.store(0, std::memory_order_release);
    } else if (current == State::HalfOpen) {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_count_.store(0, std::memory_order_release);
        half_open_probes_.store(0, std::memory_order_release);
        state_.store(State::Closed, std::memory_order_release);
    }
}

bool EndpointCircuitBreaker::allow_send() {
    State current = state_.load(std::memory_order_acquire);

    if (current == State::Closed) {
        return true;
    }

    if (current == State::HalfOpen) {
        size_t probes = half_open_probes_.fetch_add(1, std::memory_order_acq_rel);
        return probes < config_.half_open_probe_limit;
    }

    // State::Open — check cooldown
    std::lock_guard<std::mutex> lock(mutex_);
    current = state_.load(std::memory_order_acquire);
    if (current != State::Open) {
        if (current == State::Closed)
            return true;
        if (current == State::HalfOpen) {
            size_t probes =
                half_open_probes_.fetch_add(1, std::memory_order_acq_rel);
            return probes < config_.half_open_probe_limit;
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - opened_at_;
    if (elapsed >= config_.cooldown) {
        half_open_probes_.store(0, std::memory_order_release);
        state_.store(State::HalfOpen, std::memory_order_release);
        size_t probes = half_open_probes_.fetch_add(1, std::memory_order_acq_rel);
        return probes < config_.half_open_probe_limit;
    }

    return false;
}

EndpointCircuitBreaker::State EndpointCircuitBreaker::state() const {
    return state_.load(std::memory_order_acquire);
}

size_t EndpointCircuitBreaker::failure_count() const {
    return failure_count_.load(std::memory_order_acquire);
}

void EndpointCircuitBreaker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_count_.store(0, std::memory_order_release);
    half_open_probes_.store(0, std::memory_order_release);
    state_.store(State::Closed, std::memory_order_release);
}

} // namespace hpactor::net
