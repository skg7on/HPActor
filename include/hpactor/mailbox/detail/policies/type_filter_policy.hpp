#pragma once

#include <hpactor/mailbox/admission_policy.hpp>
#include <unordered_set>
#include <vector>

namespace hpactor::mailbox::detail {

/// \brief Admission policy that accepts or rejects messages based on TypeTag.
class TypeFilterPolicy : public IAdmissionPolicy {
  public:
    TypeFilterPolicy(std::vector<uint32_t> allowed_tags,
                     std::vector<uint32_t> blocked_tags) noexcept
        : allowed_(allowed_tags.begin(), allowed_tags.end()),
          blocked_(blocked_tags.begin(), blocked_tags.end()) {}

    /// \brief Evaluate whether the message TypeTag is allowed or blocked.
    ///
    /// The blocked set takes priority over the allowed set. If the allowed set
    /// is non-empty, only tags in the set are admitted. If both sets are empty,
    /// all tags are admitted.
    ///
    /// \param[in] msg The message being admitted (unused).
    /// \param[in] meta Envelope metadata with \c type_tag for classification.
    /// \param[in] config Mailbox configuration (unused).
    /// \param[in] mailbox_depth Current mailbox depth (unused).
    /// \return \c Accept if the type tag is allowed and not blocked,
    ///         \c Reject otherwise.
    /// \note Thread safety: lock-free — safe to call from any thread.
    AdmissionPolicyResult
    evaluate(const TypedMessage& msg, const MailboxEnvelopeMeta& meta,
             const MailboxConfig& config, uint64_t mailbox_depth) noexcept override {
        (void)msg;
        (void)config;
        (void)mailbox_depth;
        uint32_t tag = static_cast<uint32_t>(meta.type_tag);

        // Blocked set takes priority over allowed
        if (!blocked_.empty() && blocked_.count(tag)) {
            AdmissionPolicyResult r;
            r.decision = AdmissionDecision::Reject;
            r.policy_name = "type_filter";
            return r;
        }

        // If allowed set is non-empty, only admit matching tags
        if (!allowed_.empty() && !allowed_.count(tag)) {
            AdmissionPolicyResult r;
            r.decision = AdmissionDecision::Reject;
            r.policy_name = "type_filter";
            return r;
        }

        return {};
    }

    const char* name() const noexcept override {
        return "type_filter";
    }

  private:
    std::unordered_set<uint32_t> allowed_;
    std::unordered_set<uint32_t> blocked_;
};

} // namespace hpactor::mailbox::detail
