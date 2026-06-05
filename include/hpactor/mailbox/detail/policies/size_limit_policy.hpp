#pragma once

#include <hpactor/mailbox/admission_policy.hpp>

namespace hpactor::mailbox::detail {

/// \brief Admission policy that rejects messages exceeding size limit.
class SizeLimitPolicy : public IAdmissionPolicy {
  public:
    explicit SizeLimitPolicy(uint64_t max_bytes, bool dlq_on_reject = false) noexcept
        : max_bytes_(max_bytes), dlq_on_reject_(dlq_on_reject) {}

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
