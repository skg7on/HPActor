#pragma once

#include <hpactor/mailbox/admission_policy.hpp>

namespace hpactor::mailbox::detail {

/// \brief Admission policy that rejects messages below a minimum priority.
class PriorityThresholdPolicy : public IAdmissionPolicy {
  public:
    explicit PriorityThresholdPolicy(uint8_t min_priority,
                                     bool dlq_on_reject = false) noexcept
        : min_priority_(min_priority), dlq_on_reject_(dlq_on_reject) {}

    /// \brief Evaluate whether the message meets the minimum priority
    /// threshold.
    ///
    /// \param[in] msg The message being admitted (unused).
    /// \param[in] meta Envelope metadata with priority level for comparison.
    /// \param[in] config Mailbox configuration (unused).
    /// \param[in] mailbox_depth Current mailbox depth (unused).
    /// \return \c Accept if \c meta.priority >= \c min_priority_,
    ///         \c Reject (or \c RerouteToDLQ) otherwise.
    /// \note Thread safety: lock-free — safe to call from any thread.
    AdmissionPolicyResult
    evaluate(const TypedMessage& msg, const MailboxEnvelopeMeta& meta,
             const MailboxConfig& config, uint64_t mailbox_depth) noexcept override {
        (void)msg;
        (void)config;
        (void)mailbox_depth;
        if (meta.priority < min_priority_) {
            AdmissionPolicyResult r;
            r.decision = dlq_on_reject_ ? AdmissionDecision::RerouteToDLQ
                                        : AdmissionDecision::Reject;
            r.policy_name = "priority_threshold";
            return r;
        }
        return {};
    }

    const char* name() const noexcept override {
        return "priority_threshold";
    }

  private:
    uint8_t min_priority_;
    bool dlq_on_reject_;
};

} // namespace hpactor::mailbox::detail
