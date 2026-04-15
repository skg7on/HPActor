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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/message.hpp>
#include <hpactor/types/types.hpp>

#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// TypeTag - type identifier for serialization (replaces RTTI)
// -----------------------------------------------------------------------------
// Each serializable type gets a unique tag. System messages use tags 0-99,
// user messages use tags 100+.
// -----------------------------------------------------------------------------
enum class TypeTag : uint32_t {
    Invalid = 0,

    // System messages (always present)
    DownMsg = 1,
    ExitMsg = 2,
    LinkMsg = 3,
    UnlinkMsg = 4,

    // First available user tag
    User = 100,
};

// -----------------------------------------------------------------------------
// Serializer interface
// -----------------------------------------------------------------------------
// Provides type-tagged serialization without RTTI. Each type must be
// registered with an encode/decode function.
// -----------------------------------------------------------------------------
class Serializer {
public:
    virtual ~Serializer() = default;

    // Encode a message to bytes
    virtual bytes encode(TypeTag tag, const MessageVariant& msg) = 0;

    // Decode bytes to a message
    virtual MessageVariant decode(TypeTag tag, const bytes& data) = 0;

    // Register a user message type
    template <typename T>
    void register_type(TypeTag tag);
};

// -----------------------------------------------------------------------------
// DefaultSerializer - length-prefixed binary serialization
// -----------------------------------------------------------------------------
// Format: [4 bytes: payload length][4 bytes: type tag][payload...]
// -----------------------------------------------------------------------------
class DefaultSerializer : public Serializer {
public:
    DefaultSerializer();

    // Serializer interface
    bytes encode(TypeTag tag, const MessageVariant& msg) override;
    MessageVariant decode(TypeTag tag, const bytes& data) override;

    // Register a user message type with custom encode/decode
    using encode_func = std::function<bytes(const void*)>;
    using decode_func = std::function<void(void*, const bytes&)>;

    void register_type(TypeTag tag, encode_func encode, decode_func decode);

    template <typename T>
    void register_user_type(TypeTag tag);

private:
    bytes encode_system(const MessageVariant& msg);
    MessageVariant decode_system(TypeTag tag, const bytes& data);

    std::unordered_map<TypeTag, encode_func> encoders_;
    std::unordered_map<TypeTag, decode_func> decoders_;
};

// Template implementation
template <typename T>
void Serializer::register_type(TypeTag /*tag*/) {
    // Default implementation does nothing - specialize in DefaultSerializer
}

template <typename T>
void DefaultSerializer::register_user_type(TypeTag tag) {
    encoders_[tag] = [](const void* ptr) -> bytes {
        const T* msg = static_cast<const T*>(ptr);
        bytes result;
        // Simple length-prefixed encoding for POD types
        // TODO: implement proper serialization for complex types
        result.resize(sizeof(T));
        std::memcpy(result.data(), msg, sizeof(T));
        return result;
    };
    decoders_[tag] = [](void* ptr, const bytes& data) {
        T* msg = static_cast<T*>(ptr);
        std::memcpy(msg, data.data(), sizeof(T));
    };
}

} // namespace hpactor
