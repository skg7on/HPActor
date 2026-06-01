#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>

namespace hpactor::net {

struct EndpointCircuitBreakerConfig {
    size_t failure_threshold = 5;
    std::chrono::milliseconds cooldown{30'000};
    size_t half_open_probe_limit = 1;
};

class EndpointCircuitBreaker {
  public:
    enum class State : uint8_t { Closed, Open, HalfOpen };

    explicit EndpointCircuitBreaker(const EndpointCircuitBreakerConfig& config);

    /// Called by ConnectionPool::on_connection_error()
    void record_failure();

    /// Called by ConnectionPool::on_connection_ready()
    void record_success();

    /// Called before each try_enqueue(). Returns true if message may proceed.
    bool allow_send();

    State state() const;
    size_t failure_count() const;

    /// Force-reset to Closed
    void reset();

  private:
    EndpointCircuitBreakerConfig config_;
    std::atomic<State> state_{State::Closed};
    std::atomic<size_t> failure_count_{0};
    std::atomic<size_t> half_open_probes_{0};
    std::chrono::steady_clock::time_point opened_at_{};
    std::mutex mutex_;
};

} // namespace hpactor::net
