#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>

namespace hpactor::net {

/// \brief Configuration for the endpoint-level circuit breaker.
struct EndpointCircuitBreakerConfig {
    /// \brief Number of consecutive failures before opening (default 5).
    size_t failure_threshold = 5;
    /// \brief Cooldown period before transitioning to HalfOpen (default 30 s).
    std::chrono::milliseconds cooldown{30'000};
    /// \brief Max probe messages allowed in HalfOpen state (default 1).
    size_t half_open_probe_limit = 1;
};

/// \brief Per-endpoint circuit breaker for outbound connection failure
/// protection.
///
/// Tracks connection failures per remote endpoint and trips to \c Open
/// after \c failure_threshold consecutive failures. After \c cooldown,
/// transitions to \c HalfOpen and allows a limited number of probe
/// messages. On success, transitions back to \c Closed.
///
/// \note Thread safety: \c record_failure(), \c record_success(), and
///       \c allow_send() are safe to call from any thread. State
///       transitions use atomics with a mutex-guarded cooldown clock.
class EndpointCircuitBreaker {
  public:
    /// \brief Circuit breaker state.
    enum class State : uint8_t {
        Closed,   ///< Normal operation; all messages allowed.
        Open,     ///< Circuit tripped; no messages allowed.
        HalfOpen, ///< Probing; limited messages allowed.
    };

    /// \brief Construct with the given configuration.
    ///
    /// \param[in] config Circuit breaker configuration.
    explicit EndpointCircuitBreaker(const EndpointCircuitBreakerConfig& config);

    /// \brief Record a connection failure.
    ///
    /// Called by \c ConnectionPool::on_connection_error().
    /// \note Thread safety: Safe to call from any thread.
    void record_failure();

    /// \brief Record a successful connection.
    ///
    /// Called by \c ConnectionPool::on_connection_ready().
    /// \note Thread safety: Safe to call from any thread.
    void record_success();

    /// \brief Check whether a message may proceed.
    ///
    /// Called before each \c try_enqueue().
    /// \return \c true if the circuit allows sends.
    /// \note Thread safety: Safe to call from any thread.
    bool allow_send();

    /// \brief Return the current circuit state.
    ///
    /// \return \c Closed, \c Open, or \c HalfOpen.
    State state() const;

    /// \brief Return the current failure count.
    ///
    /// \return Number of consecutive failures recorded.
    size_t failure_count() const;

    /// \brief Force-reset the circuit to \c Closed.
    ///
    /// \note Thread safety: Safe to call from any thread.
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
