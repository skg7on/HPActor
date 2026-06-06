#pragma once

#include <hpactor/mailbox/actor_rate_limiter.hpp>
#include <hpactor/mailbox/admission_policy.hpp>

#include <mutex>
#include <unordered_map>

namespace hpactor::mailbox::detail {

/// \brief Admission policy that independently rate-limits each sender.
///
/// Maintains an independent token bucket per sender ActorId. When a sender
/// exceeds its rate, messages from that sender are rejected.
/// Stale buckets are lazily evicted when the map exceeds max_senders * 2.
class PerSenderRatePolicy : public IAdmissionPolicy {
  public:
    PerSenderRatePolicy(double rate_per_sec, uint32_t burst,
                        uint32_t max_senders = 256) noexcept
        : rate_per_sec_(rate_per_sec), burst_(burst), max_senders_(max_senders),
          max_purge_threshold_(max_senders * 2) {}

    AdmissionPolicyResult
    evaluate(const TypedMessage& msg, const MailboxEnvelopeMeta& meta,
             const MailboxConfig& config, uint64_t mailbox_depth) noexcept override {
        (void)msg;
        (void)config;
        (void)mailbox_depth;
        uint64_t sender_key = meta.sender.id.value();

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buckets_.find(sender_key);
        if (it == buckets_.end()) {
            if (buckets_.size() >= max_purge_threshold_) {
                maybe_purge_stale();
            }
            if (buckets_.size() >= max_senders_) {
                AdmissionPolicyResult r;
                r.decision = AdmissionDecision::Reject;
                r.policy_name = "per_sender_rate";
                r.match_code = 1; // max senders exceeded
                return r;
            }
            auto limiter = std::make_unique<ActorRateLimiter>();
            limiter->configure(rate_per_sec_, burst_);
            it = buckets_.emplace(sender_key, std::move(limiter)).first;
        }

        uint64_t now = now_ns();
        if (!it->second->try_consume(now)) {
            AdmissionPolicyResult r;
            r.decision = AdmissionDecision::Reject;
            r.policy_name = "per_sender_rate";
            r.match_code = 2; // rate limited
            return r;
        }
        return {};
    }

    const char* name() const noexcept override {
        return "per_sender_rate";
    }

    /// Number of active per-sender buckets (for metrics).
    size_t bucket_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return buckets_.size();
    }

  private:
    static uint64_t now_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    void maybe_purge_stale() noexcept {
        // Simple purge: remove up to 25% of oldest entries
        size_t target_remove = max_senders_ / 4;
        if (target_remove == 0)
            target_remove = 1;
        size_t removed = 0;
        auto it = buckets_.begin();
        while (it != buckets_.end() && removed < target_remove) {
            it = buckets_.erase(it);
            removed++;
        }
    }

    double rate_per_sec_;
    uint32_t burst_;
    uint32_t max_senders_;
    uint32_t max_purge_threshold_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ActorRateLimiter>> buckets_;
};

} // namespace hpactor::mailbox::detail
