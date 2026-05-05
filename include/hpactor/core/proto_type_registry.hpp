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

#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/types/types.hpp>

#include <google/protobuf/message.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// MessageTraits<T> — compile-time mapping from protobuf type to TypeTag.
//
// Primary template returns Invalid. Specializations provide the tag.
// System messages specialize with constexpr tags; user messages (registered
// via HPACTOR_MESSAGE) store a runtime-allocated tag in a function-local
// static.
//
// Usage:
//   TypeTag tag = MessageTraits<MyProtoMsg>::tag();
// -----------------------------------------------------------------------------
template <typename ProtoMsgT>
struct MessageTraits {
    static TypeTag tag() { return TypeTag::Invalid; }
};

// -----------------------------------------------------------------------------
// System message MessageTraits specializations.
// Kept in sync with the TypeTag enum system range (0x00–0xFF).
// -----------------------------------------------------------------------------
#define HPACTOR_SYSTEM_MESSAGE(ProtoMsg, TagValue)                             \
    template <> struct MessageTraits<ProtoMsg> {                               \
        static constexpr TypeTag tag() { return TagValue; }                    \
    };

// Forward-declare protobuf-generated types (defined in messages.proto, common.proto).
class DownMessage;
class ExitMessage;
class LinkMessage;
class UnlinkMessage;
class SpawnRequestMessage;
class SpawnResponseMessage;
class MetricsRequest;
class MetricsResponse;

HPACTOR_SYSTEM_MESSAGE(::hpactor::DownMessage,            TypeTag::DownMsg)
HPACTOR_SYSTEM_MESSAGE(::hpactor::ExitMessage,            TypeTag::ExitMsg)
HPACTOR_SYSTEM_MESSAGE(::hpactor::LinkMessage,            TypeTag::LinkMsg)
HPACTOR_SYSTEM_MESSAGE(::hpactor::UnlinkMessage,          TypeTag::UnlinkMsg)
HPACTOR_SYSTEM_MESSAGE(::hpactor::SpawnRequestMessage,    TypeTag::SpawnRequestTag)
HPACTOR_SYSTEM_MESSAGE(::hpactor::SpawnResponseMessage,   TypeTag::SpawnResponseTag)
HPACTOR_SYSTEM_MESSAGE(::hpactor::MetricsRequest,         TypeTag::MetricsRequestTag)
HPACTOR_SYSTEM_MESSAGE(::hpactor::MetricsResponse,        TypeTag::MetricsResponseTag)

#undef HPACTOR_SYSTEM_MESSAGE

// -----------------------------------------------------------------------------
// MessageRegistry — singleton for allocating user TypeTags.
// -----------------------------------------------------------------------------
class MessageRegistry {
public:
    static MessageRegistry& instance() {
        static MessageRegistry reg;
        return reg;
    }

    // Allocate the next user TypeTag. Called once per type at static init.
    TypeTag allocate(const std::string& /*type_name*/) {
        uint32_t tag_val = next_user_tag_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<TypeTag>(tag_val);
    }

private:
    MessageRegistry() : next_user_tag_(static_cast<uint32_t>(TypeTag::User)) {}
    std::atomic<uint32_t> next_user_tag_;
};

// -----------------------------------------------------------------------------
// HPACTOR_MESSAGE(ProtoMsg) — auto-register a user protobuf message type.
//
// Place at global scope alongside the .proto definition or in the actor
// header. At static init time, allocates a unique TypeTag from the user
// range (0x1000+) and specializes MessageTraits<ProtoMsg> so that
// type_tag_for<ProtoMsg>() returns the allocated tag.
//
// For wire deserialization support, call
//   proto_registry().register_type<ProtoMsg>(tag, name)
// during ActorSystem initialization with the tag from MessageTraits.
// -----------------------------------------------------------------------------
#define HPACTOR_MESSAGE(ProtoMsg)                                              \
    template <> struct ::hpactor::MessageTraits<ProtoMsg> {                    \
        static ::hpactor::TypeTag tag() {                                      \
            static const ::hpactor::TypeTag t = [] {                           \
                return ::hpactor::MessageRegistry::instance()                  \
                    .allocate(#ProtoMsg);                                       \
            }();                                                               \
            return t;                                                          \
        }                                                                      \
    };

// -----------------------------------------------------------------------------
// ProtoTypeRegistry - maps TypeTag values to protobuf message types.
//
// Used for the TypeTag → protobuf deserialization direction (wire protocol).
// For the reverse direction (C++ type → TypeTag), use MessageTraits<T>::tag().
//
// NOTE: Not thread-safe. Register all types during single-threaded
// initialization before any actors begin processing messages.
// -----------------------------------------------------------------------------
class ProtoTypeRegistry {
public:
    template<typename ProtoMsgT>
    void register_type(TypeTag tag, const std::string& type_name) {
        static_assert(std::is_base_of_v<google::protobuf::Message, ProtoMsgT>,
                      "ProtoMsgT must be a protobuf message type");
        Entry entry;
        entry.type_name = type_name;
        entry.prototype = mem::allocate_shared<ProtoMsgT>(
            ActorId{}, mem::RegionType::kInternal);
        registry_[tag] = std::move(entry);
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

    void register_system_types();

private:
    struct Entry {
        std::string type_name;
        std::shared_ptr<google::protobuf::Message> prototype;
    };

    std::unordered_map<TypeTag, Entry> registry_;
};

} // namespace hpactor
