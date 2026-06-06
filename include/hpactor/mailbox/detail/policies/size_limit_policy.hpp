#pragma once

#include <hpactor/mailbox/admission_policy.hpp>

namespace hpactor::mailbox::detail {

/// \brief Admission policy that rejects messages exceeding size limit.
class SizeLimitPolicy : public IAdmissionPolicy {
  public:
    explicit SizeLimitPolicy(uint64_t max_bytes, bool dlq_on_reject = false) noexcept
        : max_bytes_(max_bytes), dlq_on_reject_(dlq_on_reject) {}

    /// \brief Evaluate whether the message exceeds the size limit.
    ///
    /// \param[in] msg The message being admitted (unused).
    /// \param[in] meta Envelope metadata with \c estimated_bytes for
    /// comparison.
    /// \param[in] config Mailbox configuration (unused).
    /// \param[in] mailbox_depth Current mailbox depth (unused).
    /// \return \c Accept if \c meta.estimated_bytes <= \c max_bytes_,
    ///         \c Reject (or \c RerouteToDLQ) otherwise.
    /// \note Thread safety: lock-free — safe to call from any thread.
    AdmissionPolicyResult
    evaluate(const TypedMessage& msg, const MailboxEnvelopeMeta& meta,
             const MailboxConfig& config, uint64_t mailbox_depth) noexcept override {
        (void)msg;
        (void)config;
        (void)mailbox_depth;
        if (meta.estimated_bytes > max_bytes_) {
            AdmissionPolicyResult r;
            r.decision = dlq_on_reject_ ? AdmissionDecision::RerouteToDLQ
                                        : AdmissionDecision::Reject;
            r.policy_name = "size_limit";
            return r;
        }
        return {};
    }

    const char* name() const noexcept override {
        return "size_limit";
    }

  private:
    uint64_t max_bytes_;
    bool dlq_on_reject_;
};

} // namespace hpactor::mailbox::detail
