// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/ai/ai_type_tags.hpp>
#include <hpactor/ai_resource.pb.h>
#include <hpactor/core/proto_type_registry.hpp>

namespace hpactor::ai {
namespace {

void register_ai_message_types(ProtoTypeRegistry& reg) {
    reg.register_type<PbDeviceLeaseRequest>(kAiLeaseRequestTag,
                                            "hpactor.PbDeviceLeaseRequest");
    reg.register_type<PbDeviceLeaseReply>(kAiLeaseReplyTag,
                                          "hpactor.PbDeviceLeaseReply");
    reg.register_type<PbDeviceLeaseActivate>(kAiLeaseActivateTag,
                                             "hpactor.PbDeviceLeaseActivate");
    reg.register_type<PbDeviceLeaseRenew>(kAiLeaseRenewTag,
                                          "hpactor.PbDeviceLeaseRenew");
    reg.register_type<PbDeviceLeaseRelease>(kAiLeaseReleaseTag,
                                            "hpactor.PbDeviceLeaseRelease");
    reg.register_type<PbDeviceLeaseRevoked>(kAiLeaseRevokedTag,
                                            "hpactor.PbDeviceLeaseRevoked");
    reg.register_type<PbResourceSnapshotRequest>(
        kAiResourceSnapshotRequestTag, "hpactor.PbResourceSnapshotRequest");
    reg.register_type<PbResourceSnapshotReply>(kAiResourceSnapshotReplyTag,
                                               "hpactor.PbResourceSnapshotReply");
    reg.register_type<PbDeviceSnapshotUpdate>(kAiDeviceSnapshotUpdateTag,
                                              "hpactor.PbDeviceSnapshotUpdate");
    reg.register_type<PbNodeResourceSummary>(kAiNodeResourceSummaryTag,
                                             "hpactor.PbNodeResourceSummary");
}

const bool kAiTypesRegistered = [] {
    ProtoTypeRegistry::register_subsystem(&register_ai_message_types);
    return true;
}();

} // namespace
} // namespace hpactor::ai
