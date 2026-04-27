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

#include <hpactor/core/proto_type_registry.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

namespace hpactor {

void ProtoTypeRegistry::register_system_types() {
    register_type<::hpactor::DownMessage>(TypeTag::DownMsg, "hpactor.DownMessage");
    register_type<::hpactor::ExitMessage>(TypeTag::ExitMsg, "hpactor.ExitMessage");
    register_type<::hpactor::LinkMessage>(TypeTag::LinkMsg, "hpactor.LinkMessage");
    register_type<::hpactor::UnlinkMessage>(TypeTag::UnlinkMsg, "hpactor.UnlinkMessage");
    register_type<::hpactor::SpawnRequestMessage>(TypeTag::SpawnRequestTag,
                                                   "hpactor.SpawnRequestMessage");
    register_type<::hpactor::SpawnResponseMessage>(TypeTag::SpawnResponseTag,
                                                    "hpactor.SpawnResponseMessage");
}

} // namespace hpactor
