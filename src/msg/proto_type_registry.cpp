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

#include <hpactor/msg/proto_type_registry.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

#include <mutex>
#include <vector>

namespace hpactor {
namespace {

std::vector<ProtoTypeRegistry::SubsystemRegistrar>& subsystem_registrars() {
    static std::vector<ProtoTypeRegistry::SubsystemRegistrar> regs;
    return regs;
}

} // namespace

void ProtoTypeRegistry::register_subsystem(SubsystemRegistrar fn) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    subsystem_registrars().push_back(fn);
}

void ProtoTypeRegistry::register_system_types() {
    register_type<::hpactor::DownMessage>(TypeTag::DownMsg, "hpactor."
                                                            "DownMessage");
    register_type<::hpactor::ExitMessage>(TypeTag::ExitMsg, "hpactor."
                                                            "ExitMessage");
    register_type<::hpactor::LinkMessage>(TypeTag::LinkMsg, "hpactor."
                                                            "LinkMessage");
    register_type<::hpactor::UnlinkMessage>(TypeTag::UnlinkMsg, "hpactor."
                                                                "UnlinkMessag"
                                                                "e");
    register_type<::hpactor::SpawnRequestMessage>(TypeTag::SpawnRequestTag, "hp"
                                                                            "ac"
                                                                            "to"
                                                                            "r."
                                                                            "Sp"
                                                                            "aw"
                                                                            "nR"
                                                                            "eq"
                                                                            "ue"
                                                                            "st"
                                                                            "Me"
                                                                            "ss"
                                                                            "ag"
                                                                            "e");
    register_type<::hpactor::SpawnResponseMessage>(TypeTag::SpawnResponseTag,
                                                   "hpactor."
                                                   "SpawnResponseMessage");
    register_type<::hpactor::BackpressureSignalMessage>(
        TypeTag::BackpressureSignalTag, "hpactor.BackpressureSignalMessage");

    // ── Subsystem types (0x80–0xFF, registered via register_subsystem) ──
    for (auto fn : subsystem_registrars()) {
        fn(*this);
    }
}

} // namespace hpactor
