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

#include <atomic>
#include <cstdint>
#include <limits>

namespace hpactor::mailbox {

/// \brief Token-bucket rate limiter for actor-local message processing rate
/// control.
///
/// Designed for the consumption side of `MPSCActorMailbox`. `try_consume()` is
/// called from the consumer thread (serialized by mailbox's consumer lock), so
/// the fast-path does not require atomic RMW — relaxed atomic loads/stores are
/// used for safe reads from CLI/metrics/admin threads.
///
/// When `rate_per_sec <= 0`, the limiter is disabled and `try_consume()` always
/// returns true.
class ActorRateLimiter {
  public:
    ActorRateLimiter() noexcept = default;

    /// \brief Configure the rate limiter.
    /// \param rate_per_sec Sustained rate in messages/second. <= 0 disables.
    /// \param burst        Maximum accumulated tokens (burst allowance).
    void configure(double rate_per_sec, uint32_t burst) noexcept {
        if (rate_per_sec <= 0.0) {
            enabled_.store(false, std::memory_order_relaxed);
            rate_per_ns_ = 0.0;
            burst_ = 0;
            max_tokens_ = 0.0;
            return;
        }
        enabled_.store(true, std::memory_order_relaxed);
        rate_per_ns_ = rate_per_sec / 1'000'000'000.0;
        burst_ = burst;
        max_tokens_ = static_cast<double>(burst);
        tokens_.store(max_tokens_, std::memory_order_relaxed);
        last_refill_ns_.store(0, std::memory_order_relaxed);
    }

    /// \brief Try to consume one token.
    /// \param now_ns Current steady_clock time in nanoseconds.
    /// \return true if token available or rate limiting disabled.
    bool try_consume(uint64_t now_ns) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) {
            return true;
        }
        refill(now_ns);
        double t = tokens_.load(std::memory_order_relaxed);
        if (t >= 1.0) {
            tokens_.store(t - 1.0, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /// \brief Time until next token in nanoseconds.
    /// Returns UINT64_MAX if disabled or unlimited.
    /// Returns 0 if token available now.
    uint64_t time_until_next_token_ns(uint64_t /* now_ns */) const noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) {
            return std::numeric_limits<uint64_t>::max();
        }
        double t = tokens_.load(std::memory_order_relaxed);
        if (t >= 1.0)
            return 0;
        if (rate_per_ns_ <= 0.0)
            return std::numeric_limits<uint64_t>::max();
        double need = 1.0 - t;
        return static_cast<uint64_t>(need / rate_per_ns_);
    }

    /// \brief Current token count (for CLI / metrics snapshot).
    double current_tokens() const noexcept {
        return tokens_.load(std::memory_order_relaxed);
    }

    /// \brief Configured rate in messages/second.
    double configured_rate() const noexcept {
        if (!enabled_.load(std::memory_order_relaxed))
            return 0.0;
        return rate_per_ns_ * 1'000'000'000.0;
    }

    /// \brief Configured burst (max accumulated tokens).
    uint32_t configured_burst() const noexcept {
        return burst_;
    }

    /// \brief Whether rate limiting is enabled.
    bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

  private:
    void refill(uint64_t now_ns) noexcept {
        uint64_t last = last_refill_ns_.load(std::memory_order_relaxed);
        if (last == 0) {
            last_refill_ns_.store(now_ns, std::memory_order_relaxed);
            tokens_.store(max_tokens_, std::memory_order_relaxed);
            return;
        }
        if (now_ns <= last)
            return;
        uint64_t elapsed = now_ns - last;
        double earned = static_cast<double>(elapsed) * rate_per_ns_;
        double current = tokens_.load(std::memory_order_relaxed);
        double new_tokens = current + earned;
        if (new_tokens > max_tokens_)
            new_tokens = max_tokens_;
        tokens_.store(new_tokens, std::memory_order_relaxed);
        last_refill_ns_.store(now_ns, std::memory_order_relaxed);
    }

    std::atomic<double> tokens_{0.0};
    std::atomic<uint64_t> last_refill_ns_{0};
    double rate_per_ns_{0.0};
    uint32_t burst_{0};
    double max_tokens_{0.0};
    std::atomic<bool> enabled_{false};
};

} // namespace hpactor::mailbox
