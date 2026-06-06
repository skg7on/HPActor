#pragma once

#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/types/failure_reason.hpp>

#include <cstdint>

namespace hpactor::mailbox {

/// \brief Decision returned by an admission policy evaluation.
enum class AdmissionDecision : uint8_t {
    Accept,
    Reject,
    RerouteToDLQ,
};

/// \brief Result of a single admission policy evaluation.
struct AdmissionPolicyResult {
    AdmissionDecision decision{AdmissionDecision::Accept};
    FailureReason reason{FailureReason::RejectedByPolicy};
    const char* policy_name{nullptr};
    uint32_t match_code{0};
};

/// \brief Interface for admission policies evaluated during try_push().
class IAdmissionPolicy {
  public:
    virtual ~IAdmissionPolicy() = default;
    virtual AdmissionPolicyResult
    evaluate(const TypedMessage& msg, const MailboxEnvelopeMeta& meta,
             const MailboxConfig& config, uint64_t mailbox_depth) noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

} // namespace hpactor::mailbox
