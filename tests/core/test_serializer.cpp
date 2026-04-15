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

#include <hpactor/types/serialization.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    // Test TypeTag enum values
    assert(static_cast<uint32_t>(TypeTag::Invalid) == 0);
    assert(static_cast<uint32_t>(TypeTag::DownMsg) == 1);
    assert(static_cast<uint32_t>(TypeTag::ExitMsg) == 2);
    assert(static_cast<uint32_t>(TypeTag::LinkMsg) == 3);
    assert(static_cast<uint32_t>(TypeTag::UnlinkMsg) == 4);
    assert(static_cast<uint32_t>(TypeTag::User) == 100);

    // Test DefaultSerializer construction
    DefaultSerializer ser;

    // Test encoding/decoding down_msg
    ActorId id(42);
    ActorAddress addr(1, 0, id, 0);
    down_msg down{addr, error(123)};
    bytes encoded = ser.encode(TypeTag::DownMsg, down);
    assert(!encoded.empty());

    MessageVariant decoded = ser.decode(TypeTag::DownMsg, encoded);
    assert(std::holds_alternative<down_msg>(decoded));
    down_msg down2 = std::get<down_msg>(decoded);
    assert(down2.terminated_actor.node_id == addr.node_id);
    assert(down2.terminated_actor.id.value() == addr.id.value());
    assert(down2.reason.code() == 123);

    // Test encoding/decoding exit_msg
    exit_msg exit{addr, error(456)};
    encoded = ser.encode(TypeTag::ExitMsg, exit);
    decoded = ser.decode(TypeTag::ExitMsg, encoded);
    assert(std::holds_alternative<exit_msg>(decoded));
    exit_msg exit2 = std::get<exit_msg>(decoded);
    assert(exit2.sender.node_id == addr.node_id);
    assert(exit2.reason.code() == 456);

    // Test encoding/decoding link_msg
    link_msg link{addr};
    encoded = ser.encode(TypeTag::LinkMsg, link);
    decoded = ser.decode(TypeTag::LinkMsg, encoded);
    assert(std::holds_alternative<link_msg>(decoded));
    link_msg link2 = std::get<link_msg>(decoded);
    assert(link2.target.node_id == addr.node_id);

    // Test encoding/decoding unlink_msg
    unlink_msg unlink{addr};
    encoded = ser.encode(TypeTag::UnlinkMsg, unlink);
    decoded = ser.decode(TypeTag::UnlinkMsg, encoded);
    assert(std::holds_alternative<unlink_msg>(decoded));
    unlink_msg unlink2 = std::get<unlink_msg>(decoded);
    assert(unlink2.target.node_id == addr.node_id);

    return 0;
}
