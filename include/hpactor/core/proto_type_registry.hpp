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

#pragma once

#include <hpactor/types/types.hpp>

#include <google/protobuf/message.h>

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// ProtoTypeRegistry - maps TypeTag values to protobuf message types
// -----------------------------------------------------------------------------
// Enables automatic serialization and deserialization of protobuf messages
// using TypeTag identifiers, avoiding RTTI. Supports wire encoding that
// prepends a 4-byte big-endian TypeTag to the protobuf payload.
//
// NOTE: Not thread-safe. Register all types during single-threaded
// initialization before any actors begin processing messages.
// -----------------------------------------------------------------------------
class ProtoTypeRegistry {
public:
    // Register a protobuf message type with a TypeTag.
    template<typename ProtoMsgT>
    void register_type(TypeTag tag, const std::string& type_name) {
        static_assert(std::is_base_of_v<google::protobuf::Message, ProtoMsgT>,
                      "ProtoMsgT must be a protobuf message type");
        Entry entry;
        entry.type_name = type_name;
        entry.prototype = std::make_shared<ProtoMsgT>();
        registry_[tag] = std::move(entry);
    }

    // Look up the TypeTag for a registered protobuf message type.
    // Returns TypeTag::Invalid if the type is not registered.
    template<typename ProtoMsgT>
    TypeTag lookup() const {
        for (const auto& [tag, entry] : registry_) {
            if (entry.prototype &&
                entry.prototype->GetTypeName() == ProtoMsgT().GetTypeName()) {
                return tag;
            }
        }
        return TypeTag::Invalid;
    }

    [[nodiscard]] bool has_tag(TypeTag tag) const {
        return registry_.find(tag) != registry_.end();
    }

    [[nodiscard]] std::string type_name(TypeTag tag) const {
        auto it = registry_.find(tag);
        if (it != registry_.end()) return it->second.type_name;
        return {};
    }

    [[nodiscard]] std::unique_ptr<google::protobuf::Message> create(TypeTag tag) const {
        auto it = registry_.find(tag);
        if (it == registry_.end() || !it->second.prototype) return nullptr;
        return std::unique_ptr<google::protobuf::Message>(
            it->second.prototype->New());
    }

    [[nodiscard]] std::unique_ptr<google::protobuf::Message> deserialize(
        TypeTag tag, const StreamBuffer& data) const {
        auto msg = create(tag);
        if (!msg) return nullptr;
        if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            return nullptr;
        }
        return msg;
    }

    [[nodiscard]] StreamBuffer serialize(const google::protobuf::Message& msg) const {
        auto size = msg.ByteSizeLong();
        StreamBuffer result(size);
        if (!msg.SerializeToArray(result.data(), static_cast<int>(size))) {
            result.clear();
        }
        return result;
    }

    // Encode TypeTag + payload into a single byte buffer:
    // [4 bytes: TypeTag big-endian][protobuf payload bytes]
    [[nodiscard]] StreamBuffer encode_wire(TypeTag tag, const google::protobuf::Message& msg) const {
        StreamBuffer payload = serialize(msg);
        StreamBuffer result(payload.size() + 4);
        uint32_t tag_val = static_cast<uint32_t>(tag);
        result[0] = static_cast<uint8_t>((tag_val >> 24) & 0xFF);
        result[1] = static_cast<uint8_t>((tag_val >> 16) & 0xFF);
        result[2] = static_cast<uint8_t>((tag_val >> 8) & 0xFF);
        result[3] = static_cast<uint8_t>(tag_val & 0xFF);
        if (!payload.empty()) {
            std::memcpy(result.data() + 4, payload.data(), payload.size());
        }
        return result;
    }

    [[nodiscard]] std::pair<TypeTag, std::unique_ptr<google::protobuf::Message>>
    decode_wire(const StreamBuffer& data) const {
        if (data.size() < 4) return {TypeTag::Invalid, nullptr};
        uint32_t tag_val =
            (static_cast<uint32_t>(data[0]) << 24) |
            (static_cast<uint32_t>(data[1]) << 16) |
            (static_cast<uint32_t>(data[2]) << 8) |
            static_cast<uint32_t>(data[3]);
        TypeTag tag = static_cast<TypeTag>(tag_val);
        StreamBuffer payload(data.begin() + 4, data.end());
        auto msg = deserialize(tag, payload);
        return {tag, std::move(msg)};
    }

    // Pre-register all system message types with well-known TypeTags.
    void register_system_types();

private:
    struct Entry {
        std::string type_name;
        std::shared_ptr<google::protobuf::Message> prototype;
    };

    std::unordered_map<TypeTag, Entry> registry_;
};

} // namespace hpactor
