#pragma once

#include <hpactor/mailbox/admission_policy.hpp>
#include <unordered_set>
#include <vector>

namespace hpactor::mailbox::detail {

/// \brief Admission policy that accepts or rejects messages based on sender.
class SenderFilterPolicy : public IAdmissionPolicy {
  public:
    explicit SenderFilterPolicy(std::vector<uint64_t> blocked_ids) noexcept
        : blocked_(blocked_ids.begin(), blocked_ids.end()) {}

    /// \brief Evaluate whether the sender is in the blocked set.
    ///
    /// \param[in] msg The message being admitted (unused).
    /// \param[in] meta Envelope metadata with sender ActorId for lookup.
    /// \param[in] config Mailbox configuration (unused).
    /// \param[in] mailbox_depth Current mailbox depth (unused).
    /// \return \c Accept if the sender is not in the blocked set,
    ///         \c Reject otherwise.
    /// \note Thread safety: lock-free — safe to call from any thread.
    AdmissionPolicyResult
    evaluate(const TypedMessage& msg, const MailboxEnvelopeMeta& meta,
             const MailboxConfig& config, uint64_t mailbox_depth) noexcept override {
        (void)msg;
        (void)config;
        (void)mailbox_depth;
        uint64_t sender_id = meta.sender.id.value();
        if (!blocked_.empty() && blocked_.count(sender_id)) {
            AdmissionPolicyResult r;
            r.decision = AdmissionDecision::Reject;
            r.policy_name = "sender_filter";
            return r;
        }
        return {};
    }

    const char* name() const noexcept override {
        return "sender_filter";
    }

  private:
    std::unordered_set<uint64_t> blocked_;
};

} // namespace hpactor::mailbox::detail
