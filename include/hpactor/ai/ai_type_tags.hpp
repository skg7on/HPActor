// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/msg/type_tag.hpp>

namespace hpactor::ai {

// ── AI resource message tags (0x80–0x89) ──────────────────────────────
inline constexpr TypeTag kAiLeaseRequestTag = make_subsystem_tag(0x80);
inline constexpr TypeTag kAiLeaseReplyTag = make_subsystem_tag(0x81);
inline constexpr TypeTag kAiLeaseActivateTag = make_subsystem_tag(0x82);
inline constexpr TypeTag kAiLeaseRenewTag = make_subsystem_tag(0x83);
inline constexpr TypeTag kAiLeaseReleaseTag = make_subsystem_tag(0x84);
inline constexpr TypeTag kAiLeaseRevokedTag = make_subsystem_tag(0x85);
inline constexpr TypeTag kAiResourceSnapshotRequestTag = make_subsystem_tag(0x86);
inline constexpr TypeTag kAiResourceSnapshotReplyTag = make_subsystem_tag(0x87);
inline constexpr TypeTag kAiDeviceSnapshotUpdateTag = make_subsystem_tag(0x88);
inline constexpr TypeTag kAiNodeResourceSummaryTag = make_subsystem_tag(0x89);

} // namespace hpactor::ai

// ── Forward-declare proto types ────────────────────────────────────────
namespace hpactor {
class PbDeviceLeaseRequest;
class PbDeviceLeaseReply;
class PbDeviceLeaseActivate;
class PbDeviceLeaseRenew;
class PbDeviceLeaseRelease;
class PbDeviceLeaseRevoked;
class PbResourceSnapshotRequest;
class PbResourceSnapshotReply;
class PbDeviceSnapshotUpdate;
class PbNodeResourceSummary;
} // namespace hpactor

// ── MessageTraits specializations ──────────────────────────────────────
namespace hpactor {

template <> struct MessageTraits<PbDeviceLeaseRequest> {
    static constexpr TypeTag tag() {
        return ai::kAiLeaseRequestTag;
    }
};
template <> struct MessageTraits<PbDeviceLeaseReply> {
    static constexpr TypeTag tag() {
        return ai::kAiLeaseReplyTag;
    }
};
template <> struct MessageTraits<PbDeviceLeaseActivate> {
    static constexpr TypeTag tag() {
        return ai::kAiLeaseActivateTag;
    }
};
template <> struct MessageTraits<PbDeviceLeaseRenew> {
    static constexpr TypeTag tag() {
        return ai::kAiLeaseRenewTag;
    }
};
template <> struct MessageTraits<PbDeviceLeaseRelease> {
    static constexpr TypeTag tag() {
        return ai::kAiLeaseReleaseTag;
    }
};
template <> struct MessageTraits<PbDeviceLeaseRevoked> {
    static constexpr TypeTag tag() {
        return ai::kAiLeaseRevokedTag;
    }
};
template <> struct MessageTraits<PbResourceSnapshotRequest> {
    static constexpr TypeTag tag() {
        return ai::kAiResourceSnapshotRequestTag;
    }
};
template <> struct MessageTraits<PbResourceSnapshotReply> {
    static constexpr TypeTag tag() {
        return ai::kAiResourceSnapshotReplyTag;
    }
};
template <> struct MessageTraits<PbDeviceSnapshotUpdate> {
    static constexpr TypeTag tag() {
        return ai::kAiDeviceSnapshotUpdateTag;
    }
};
template <> struct MessageTraits<PbNodeResourceSummary> {
    static constexpr TypeTag tag() {
        return ai::kAiNodeResourceSummaryTag;
    }
};

} // namespace hpactor
