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
#include <hpactor/spawn.hpp>

#include <cstring>

namespace hpactor {

// -----------------------------------------------------------------------------
// DefaultSerializer implementation
// -----------------------------------------------------------------------------

DefaultSerializer::DefaultSerializer() {
    // Register system message encoders/decoders
    // These are handled via std::visit in encode_system/decode_system
}

bytes DefaultSerializer::encode(TypeTag tag, const MessageVariant& msg) {
    // Check if it's a system message
    if (static_cast<uint32_t>(tag) < static_cast<uint32_t>(TypeTag::User)) {
        return encode_system(msg);
    }

    // User type - use registered encoder
    auto it = encoders_.find(tag);
    if (it != encoders_.end()) {
        bytes result;
        std::visit([&result, tag, this, &msg](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, down_msg> ||
                        std::is_same_v<T, exit_msg> ||
                        std::is_same_v<T, link_msg> ||
                        std::is_same_v<T, unlink_msg>) {
                result = encode_system(msg);
            } else {
                // User type - use encoder if registered
                auto encoder_it = encoders_.find(tag);
                if (encoder_it != encoders_.end()) {
                    result = encoder_it->second(&m);
                }
            }
        }, msg);
        return result;
    }

    return bytes{};
}

MessageVariant DefaultSerializer::decode(TypeTag tag, const bytes& data) {
    // Check if it's a system message
    if (static_cast<uint32_t>(tag) < static_cast<uint32_t>(TypeTag::User)) {
        return decode_system(tag, data);
    }

    // User type - use registered decoder
    auto it = decoders_.find(tag);
    if (it != decoders_.end()) {
        // For now, return empty - actual decode requires knowing the type
        // This will be implemented when we have proper type erasure
    }

    return MessageVariant{};
}

void DefaultSerializer::register_type(TypeTag tag, encode_func encode, decode_func decode) {
    encoders_[tag] = std::move(encode);
    decoders_[tag] = std::move(decode);
}

bytes DefaultSerializer::encode_system(const MessageVariant& msg) {
    bytes result;

    // down_msg
    if (std::holds_alternative<down_msg>(msg)) {
        const down_msg& m = std::get<down_msg>(msg);
        result.resize(sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t));
        size_t offset = 0;
        uint32_t node_id = m.terminated_actor.node_id;
        uint64_t actor_id = m.terminated_actor.id.value();
        uint32_t code = m.reason.code();
        std::memcpy(result.data() + offset, &node_id, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &actor_id, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(result.data() + offset, &code, sizeof(uint32_t));
    }
    // exit_msg
    else if (std::holds_alternative<exit_msg>(msg)) {
        const exit_msg& m = std::get<exit_msg>(msg);
        result.resize(sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t));
        size_t offset = 0;
        uint32_t node_id = m.sender.node_id;
        uint64_t actor_id = m.sender.id.value();
        uint32_t code = m.reason.code();
        std::memcpy(result.data() + offset, &node_id, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &actor_id, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(result.data() + offset, &code, sizeof(uint32_t));
    }
    // link_msg
    else if (std::holds_alternative<link_msg>(msg)) {
        const link_msg& m = std::get<link_msg>(msg);
        result.resize(sizeof(uint32_t) + sizeof(uint64_t));
        size_t offset = 0;
        uint32_t node_id = m.target.node_id;
        uint64_t actor_id = m.target.id.value();
        std::memcpy(result.data() + offset, &node_id, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &actor_id, sizeof(uint64_t));
    }
    // unlink_msg
    else if (std::holds_alternative<unlink_msg>(msg)) {
        const unlink_msg& m = std::get<unlink_msg>(msg);
        result.resize(sizeof(uint32_t) + sizeof(uint64_t));
        size_t offset = 0;
        uint32_t node_id = m.target.node_id;
        uint64_t actor_id = m.target.id.value();
        std::memcpy(result.data() + offset, &node_id, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &actor_id, sizeof(uint64_t));
    }

    return result;
}

MessageVariant DefaultSerializer::decode_system(TypeTag tag, const bytes& data) {
    switch (tag) {
    case TypeTag::DownMsg: {
        down_msg m;
        size_t offset = 0;
        uint32_t node_id;
        uint64_t actor_id;
        uint32_t code;
        std::memcpy(&node_id, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&actor_id, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(&code, data.data() + offset, sizeof(uint32_t));
        m.terminated_actor.node_id = node_id;
        m.terminated_actor.id = ActorId(actor_id);
        m.reason = error(code);
        return m;
    }
    case TypeTag::ExitMsg: {
        exit_msg m;
        size_t offset = 0;
        uint32_t node_id;
        uint64_t actor_id;
        uint32_t code;
        std::memcpy(&node_id, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&actor_id, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(&code, data.data() + offset, sizeof(uint32_t));
        m.sender.node_id = node_id;
        m.sender.id = ActorId(actor_id);
        m.reason = error(code);
        return m;
    }
    case TypeTag::LinkMsg: {
        link_msg m;
        size_t offset = 0;
        uint32_t node_id;
        uint64_t actor_id;
        std::memcpy(&node_id, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&actor_id, data.data() + offset, sizeof(uint64_t));
        m.target.node_id = node_id;
        m.target.id = ActorId(actor_id);
        return m;
    }
    case TypeTag::UnlinkMsg: {
        unlink_msg m;
        size_t offset = 0;
        uint32_t node_id;
        uint64_t actor_id;
        std::memcpy(&node_id, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&actor_id, data.data() + offset, sizeof(uint64_t));
        m.target.node_id = node_id;
        m.target.id = ActorId(actor_id);
        return m;
    }
    default:
        return MessageVariant{};
    }
}

bytes DefaultSerializer::encode_spawn([[maybe_unused]] TypeTag tag, const SpawnMessageVariant& msg) {
    bytes result;

    // SpawnRequest
    if (std::holds_alternative<SpawnRequest>(msg)) {
        const SpawnRequest& m = std::get<SpawnRequest>(msg);
        // Encode actor_type_name length + string
        size_t name_len = m.actor_type_name.size();
        result.resize(sizeof(uint32_t) + name_len + sizeof(TypeTag) +
                      sizeof(size_t) + m.serialized_args.size() +
                      sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t));
        size_t offset = 0;

        // actor_type_name length
        uint32_t name_len_u32 = static_cast<uint32_t>(name_len);
        std::memcpy(result.data() + offset, &name_len_u32, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // actor_type_name string
        std::memcpy(result.data() + offset, m.actor_type_name.data(), name_len);
        offset += name_len;

        // args_type
        uint32_t args_type = static_cast<uint32_t>(m.args_type);
        std::memcpy(result.data() + offset, &args_type, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // serialized_args length
        uint32_t args_len = static_cast<uint32_t>(m.serialized_args.size());
        std::memcpy(result.data() + offset, &args_len, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // serialized_args data
        if (!m.serialized_args.empty()) {
            std::memcpy(result.data() + offset, m.serialized_args.data(), m.serialized_args.size());
            offset += m.serialized_args.size();
        }

        // supervisor_addr fields
        uint32_t sup_node_id = m.supervisor_addr.node_id;
        uint32_t sup_type = m.supervisor_addr.type;
        uint64_t sup_actor_id = m.supervisor_addr.id.value();
        uint64_t sup_incarnation = m.supervisor_addr.incarnation;

        std::memcpy(result.data() + offset, &sup_node_id, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &sup_type, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &sup_actor_id, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(result.data() + offset, &sup_incarnation, sizeof(uint64_t));
    }
    // SpawnResponse
    else if (std::holds_alternative<SpawnResponse>(msg)) {
        const SpawnResponse& m = std::get<SpawnResponse>(msg);
        result.resize(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t));
        size_t offset = 0;

        uint32_t actor_node_id = m.actor_addr.node_id;
        uint32_t actor_type = m.actor_addr.type;
        uint64_t actor_id = m.actor_addr.id.value();
        uint64_t actor_incarnation = m.actor_addr.incarnation;
        uint32_t error_code = m.error_code;

        std::memcpy(result.data() + offset, &actor_node_id, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &actor_type, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(result.data() + offset, &actor_id, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(result.data() + offset, &actor_incarnation, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(result.data() + offset, &error_code, sizeof(uint32_t));
    }

    return result;
}

SpawnMessageVariant DefaultSerializer::decode_spawn(TypeTag tag, const bytes& data) {
    switch (tag) {
    case TypeTag::SpawnRequestTag: {
        SpawnRequest m;
        size_t offset = 0;

        // actor_type_name length
        uint32_t name_len;
        std::memcpy(&name_len, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // actor_type_name string
        m.actor_type_name.resize(name_len);
        std::memcpy(m.actor_type_name.data(), data.data() + offset, name_len);
        offset += name_len;

        // args_type
        uint32_t args_type_val;
        std::memcpy(&args_type_val, data.data() + offset, sizeof(uint32_t));
        m.args_type = static_cast<TypeTag>(args_type_val);
        offset += sizeof(uint32_t);

        // serialized_args length
        uint32_t args_len;
        std::memcpy(&args_len, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // serialized_args data
        m.serialized_args.resize(args_len);
        if (args_len > 0) {
            std::memcpy(m.serialized_args.data(), data.data() + offset, args_len);
            offset += args_len;
        }

        // supervisor_addr fields
        uint32_t sup_node_id, sup_type;
        uint64_t sup_actor_id, sup_incarnation;
        std::memcpy(&sup_node_id, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&sup_type, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&sup_actor_id, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(&sup_incarnation, data.data() + offset, sizeof(uint64_t));

        m.supervisor_addr.node_id = sup_node_id;
        m.supervisor_addr.type = sup_type;
        m.supervisor_addr.id = ActorId(sup_actor_id);
        m.supervisor_addr.incarnation = sup_incarnation;

        return m;
    }
    case TypeTag::SpawnResponseTag: {
        SpawnResponse m;
        size_t offset = 0;

        uint32_t actor_node_id, actor_type;
        uint64_t actor_id, actor_incarnation;
        uint32_t error_code;

        std::memcpy(&actor_node_id, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&actor_type, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(&actor_id, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(&actor_incarnation, data.data() + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        std::memcpy(&error_code, data.data() + offset, sizeof(uint32_t));

        m.actor_addr.node_id = actor_node_id;
        m.actor_addr.type = actor_type;
        m.actor_addr.id = ActorId(actor_id);
        m.actor_addr.incarnation = actor_incarnation;
        m.error_code = error_code;

        return m;
    }
    default:
        return SpawnMessageVariant{};
    }
}

} // namespace hpactor
