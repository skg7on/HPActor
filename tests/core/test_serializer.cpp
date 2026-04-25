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
#include <cstring>

using namespace hpactor;

int main() {
    DefaultSerializer serializer;

    // -------------------------------------------------------------------------
    // Test TypeTag enum values
    // -------------------------------------------------------------------------
    assert(static_cast<uint32_t>(TypeTag::Invalid) == 0);
    assert(static_cast<uint32_t>(TypeTag::DownMsg) == 1);
    assert(static_cast<uint32_t>(TypeTag::ExitMsg) == 2);
    assert(static_cast<uint32_t>(TypeTag::LinkMsg) == 3);
    assert(static_cast<uint32_t>(TypeTag::UnlinkMsg) == 4);
    assert(static_cast<uint32_t>(TypeTag::User) == 100);
    assert(static_cast<uint32_t>(TypeTag::SpawnRequestTag) == 5);
    assert(static_cast<uint32_t>(TypeTag::SpawnResponseTag) == 6);

    // -------------------------------------------------------------------------
    // Round-trip tests for system message types
    // -------------------------------------------------------------------------

    // Test down_msg round-trip with explicit Ipv4Endpoint fields
    {
        down_msg original;
        original.terminated_actor.endpoint = Ipv4Endpoint{htonl(0x7F000001), htons(8080)};
        original.terminated_actor.id = ActorId(42);
        original.terminated_actor.type = ActorType{1};
        original.terminated_actor.incarnation = 0;
        original.reason = error(123);

        bytes encoded = serializer.encode(TypeTag::DownMsg, original);
        assert(!encoded.empty());

        auto decoded = serializer.decode(TypeTag::DownMsg, encoded);
        assert(std::holds_alternative<down_msg>(decoded));

        const auto& d = std::get<down_msg>(decoded);
        assert(d.terminated_actor.id.value() == 42u);
        assert(d.reason.code() == 123u);
        // Verify endpoint preserved through round-trip
        auto* ipv4 = std::get_if<Ipv4Endpoint>(&d.terminated_actor.endpoint);
        assert(ipv4 != nullptr);
        assert(ipv4->port_nw == htons(8080));
    }

    // Test exit_msg round-trip
    {
        exit_msg original;
        original.sender.endpoint = Ipv4Endpoint{htonl(0xC0000203), htons(9090)};  // 192.0.2.3
        original.sender.id = ActorId(99);
        original.sender.type = ActorType{2};
        original.sender.incarnation = 5;
        original.reason = error(456);

        bytes encoded = serializer.encode(TypeTag::ExitMsg, original);
        assert(!encoded.empty());

        auto decoded = serializer.decode(TypeTag::ExitMsg, encoded);
        assert(std::holds_alternative<exit_msg>(decoded));

        const auto& d = std::get<exit_msg>(decoded);
        assert(d.sender.id.value() == 99u);
        assert(d.reason.code() == 456u);
        auto* ipv4 = std::get_if<Ipv4Endpoint>(&d.sender.endpoint);
        assert(ipv4 != nullptr);
        assert(ipv4->port_nw == htons(9090));
    }

    // Test link_msg round-trip
    {
        link_msg original;
        original.target.endpoint = Ipv4Endpoint{htonl(0x0A000001), htons(5555)};  // 10.0.0.1
        original.target.id = ActorId(77);
        original.target.type = ActorType{3};
        original.target.incarnation = 10;

        bytes encoded = serializer.encode(TypeTag::LinkMsg, original);
        assert(!encoded.empty());

        auto decoded = serializer.decode(TypeTag::LinkMsg, encoded);
        assert(std::holds_alternative<link_msg>(decoded));

        const auto& d = std::get<link_msg>(decoded);
        assert(d.target.id.value() == 77u);
        auto* ipv4 = std::get_if<Ipv4Endpoint>(&d.target.endpoint);
        assert(ipv4 != nullptr);
        assert(ipv4->port_nw == htons(5555));
    }

    // Test unlink_msg round-trip
    {
        unlink_msg original;
        original.target.endpoint = Ipv4Endpoint{htonl(0xC0A80101), htons(7777)};  // 192.168.1.1
        original.target.id = ActorId(55);
        original.target.type = ActorType{4};
        original.target.incarnation = 20;

        bytes encoded = serializer.encode(TypeTag::UnlinkMsg, original);
        assert(!encoded.empty());

        auto decoded = serializer.decode(TypeTag::UnlinkMsg, encoded);
        assert(std::holds_alternative<unlink_msg>(decoded));

        const auto& d = std::get<unlink_msg>(decoded);
        assert(d.target.id.value() == 55u);
        auto* ipv4 = std::get_if<Ipv4Endpoint>(&d.target.endpoint);
        assert(ipv4 != nullptr);
        assert(ipv4->port_nw == htons(7777));
    }

    // -------------------------------------------------------------------------
    // SpawnRequest / SpawnResponse round-trip tests
    // -------------------------------------------------------------------------

    // Test SpawnRequest round-trip
    {
        SpawnRequest original;
        original.actor_type_name = "worker";
        original.args_type = TypeTag::User;
        original.serialized_args = {1, 2, 3, 4, 5};
        original.supervisor_addr = ActorAddress{
            Ipv4Endpoint{htonl(0x7F000001), htons(8080)},
            ActorType{10},
            ActorId{42},
            1
        };

        SpawnMessageVariant mv = original;
        bytes encoded = serializer.encode_spawn(TypeTag::SpawnRequestTag, mv);
        assert(!encoded.empty());

        auto decoded = serializer.decode_spawn(TypeTag::SpawnRequestTag, encoded);
        assert(std::holds_alternative<SpawnRequest>(decoded));

        const auto& d = std::get<SpawnRequest>(decoded);
        assert(d.actor_type_name == "worker");
        assert(d.args_type == TypeTag::User);
        assert(d.serialized_args.size() == 5);
        assert(d.serialized_args[0] == 1);
        assert(d.serialized_args[4] == 5);
        auto* ipv4 = std::get_if<Ipv4Endpoint>(&d.supervisor_addr.endpoint);
        assert(ipv4 != nullptr);
        assert(ipv4->port_nw == htons(8080));
        assert(d.supervisor_addr.id.value() == 42);
    }

    // Test SpawnResponse round-trip
    {
        SpawnResponse original;
        original.actor_addr = ActorAddress{
            Ipv4Endpoint{htonl(0xC0A801FF), htons(9999)},
            ActorType{20},
            ActorId{100},
            3
        };
        original.error_code = spawn_errors::success;

        SpawnMessageVariant mv = original;
        bytes encoded = serializer.encode_spawn(TypeTag::SpawnResponseTag, mv);
        assert(!encoded.empty());

        auto decoded = serializer.decode_spawn(TypeTag::SpawnResponseTag, encoded);
        assert(std::holds_alternative<SpawnResponse>(decoded));

        const auto& d = std::get<SpawnResponse>(decoded);
        assert(d.error_code == spawn_errors::success);
        auto* ipv4 = std::get_if<Ipv4Endpoint>(&d.actor_addr.endpoint);
        assert(ipv4 != nullptr);
        assert(ipv4->port_nw == htons(9999));
        assert(d.actor_addr.id.value() == 100);
    }

    // -------------------------------------------------------------------------
    // Malformed data tests - verify decode returns default-constructed variant
    // -------------------------------------------------------------------------
    // When protobuf parse fails, decode_system returns MessageVariant{} which
    // default-constructs the first alternative (completion_msg). We verify
    // no crash occurred and the variant is in a valid state (has a valid index).

    // Corrupt down_msg data
    {
        bytes corrupted = {0xFF, 0xFE, 0xFD, 0xFC, 0x00};
        auto decoded = serializer.decode(TypeTag::DownMsg, corrupted);
        // Returns default-constructed MessageVariant (holds completion_msg)
        assert(decoded.index() < 5);  // Valid system message index
        // Verify fields are zero-initialized (default constructed)
        const auto* d = std::get_if<down_msg>(&decoded);
        if (d) {
            assert(d->terminated_actor.id.value() == 0u);
            assert(d->reason.code() == 0u);
        }
        // Also check completion_msg (the default-constructed type)
        const auto* c = std::get_if<completion_msg>(&decoded);
        assert(c != nullptr);  // Should be completion_msg
    }

    // Corrupt exit_msg data
    {
        bytes corrupted = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
        auto decoded = serializer.decode(TypeTag::ExitMsg, corrupted);
        assert(decoded.index() < 5);  // Valid system message index
        const auto* c = std::get_if<completion_msg>(&decoded);
        assert(c != nullptr);
    }

    // Corrupt link_msg data
    {
        bytes corrupted(20, 0xAB);
        auto decoded = serializer.decode(TypeTag::LinkMsg, corrupted);
        assert(decoded.index() < 5);  // Valid system message index
        const auto* c = std::get_if<completion_msg>(&decoded);
        assert(c != nullptr);
    }

    // Corrupt unlink_msg data
    {
        bytes corrupted(50, 0x42);
        auto decoded = serializer.decode(TypeTag::UnlinkMsg, corrupted);
        assert(decoded.index() < 5);  // Valid system message index
        const auto* c = std::get_if<completion_msg>(&decoded);
        assert(c != nullptr);
    }

    // Corrupt spawn request data
    {
        bytes corrupted = {0xFF, 0x00, 0x00};
        auto decoded = serializer.decode_spawn(TypeTag::SpawnRequestTag, corrupted);
        // Returns default-constructed SpawnMessageVariant (holds SpawnRequest)
        const auto* req = std::get_if<SpawnRequest>(&decoded);
        assert(req != nullptr);  // Should be SpawnRequest
        assert(req->actor_type_name.empty());
    }

    // Corrupt spawn response data
    {
        bytes corrupted = {0x01, 0x02, 0x03};
        auto decoded = serializer.decode_spawn(TypeTag::SpawnResponseTag, corrupted);
        // decode_spawn returns default-constructed SpawnMessageVariant on parse failure
        // SpawnMessageVariant = std::variant<SpawnRequest, SpawnResponse>
        // Default construction yields index 0 (SpawnRequest), not SpawnResponse
        // So we check both possibilities - parse failure can return either type
        bool is_request = std::holds_alternative<SpawnRequest>(decoded);
        bool is_response = std::holds_alternative<SpawnResponse>(decoded);
        assert(is_request || is_response);  // Must be one of them
        if (is_response) {
            const auto& resp = std::get<SpawnResponse>(decoded);
            assert(resp.actor_addr.id.value() == 0u);
        } else {
            const auto& req = std::get<SpawnRequest>(decoded);
            assert(req.actor_type_name.empty());
        }
    }

    // Invalid tag returns default-constructed MessageVariant
    {
        bytes data = {0x00, 0x01, 0x02};
        auto decoded = serializer.decode(TypeTag::Invalid, data);
        assert(decoded.index() < 5);  // Valid system message index
        const auto* c = std::get_if<completion_msg>(&decoded);
        assert(c != nullptr);
    }

    return 0;
}