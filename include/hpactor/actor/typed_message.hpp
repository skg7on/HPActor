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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <google/protobuf/message.h>

#include <atomic>
#include <memory>
#include <utility>

namespace hpactor {

// TypedMessage — the universal message carrier for all actor communication.
//
// Carries a TypeTag for dispatch routing, serialized protobuf bytes for wire
// transfer, and an optional shared_ptr<google::protobuf::Message> for zero-copy
// local delivery. The mpsc_next field provides the intrusive link for lock-free
// MPSC mailboxes.
//
// Lifecycle:
//   Local send:  TypedMessage(tag, parsed_msg, serialized_bytes)
//   Remote recv: TypedMessage(tag, serialized_bytes)
//   Lazy parse:  msg.as<ConcreteProtoMsg>() on first access

class TypedMessage {
public:
    TypedMessage() = default;

    // Copy is disabled (atomic member deletes it).
    TypedMessage(const TypedMessage&) = delete;
    TypedMessage& operator=(const TypedMessage&) = delete;

    // Move is allowed.
    TypedMessage(TypedMessage&& other) noexcept
        : tag_(other.tag_),
          payload_(std::move(other.payload_)),
          parsed_(std::move(other.parsed_)),
          sender_address_(other.sender_address_) {
        // mpsc_next is left default-initialized in the moved-from object
    }
    TypedMessage& operator=(TypedMessage&& other) noexcept {
        tag_ = other.tag_;
        payload_ = std::move(other.payload_);
        parsed_ = std::move(other.parsed_);
        sender_address_ = other.sender_address_;
        // mpsc_next intentionally not touched — ownership transferred
        return *this;
    }

    // Local send: carries both parsed form (zero-copy) and pre-serialized form.
    TypedMessage(TypeTag tag, std::shared_ptr<google::protobuf::Message> msg,
                 StreamBuffer serialized)
        : tag_(tag), payload_(std::move(serialized)),
          parsed_(std::move(msg)) {}

    // Remote receive / serialized only: payload is present, parsed is lazily
    // populated via as<T>().
    explicit TypedMessage(TypeTag tag, StreamBuffer payload)
        : tag_(tag), payload_(std::move(payload)) {}

    // Convenience: construct from a protobuf message, serializing eagerly.
    TypedMessage(TypeTag tag, const google::protobuf::Message& msg);

    TypeTag type_id() const noexcept { return tag_; }
    const StreamBuffer& payload() const noexcept { return payload_; }

    // Non-null when the message is available in parsed form (local fast path).
    std::shared_ptr<google::protobuf::Message> parsed() const noexcept {
        return parsed_;
    }

    // Lazy deserialize into a concrete protobuf type. Caches result in parsed_.
    // Returns nullptr if payload_ is empty or parsing fails.
    template <typename T>
    std::shared_ptr<T> as() const {
        if (parsed_) {
            return std::static_pointer_cast<T>(parsed_);
        }
        if (payload_.empty()) {
            return nullptr;
        }
        auto msg = std::make_shared<T>();
        if (!msg->ParseFromArray(payload_.data(),
                                 static_cast<int>(payload_.size()))) {
            return nullptr;
        }
        parsed_ = msg;
        return msg;
    }

    // Sender address — set by ActorContext::send() (local) or deliver_remote() (remote).
    // Read by EventBasedActor::receive() to populate current_sender_ for reply().
    const ActorAddress& sender_address() const noexcept { return sender_address_; }
    void set_sender_address(const ActorAddress& addr) { sender_address_ = addr; }

    // MPSC mailbox intrusive link. Must be named mpsc_next for MPSCMailbox<T>.
    std::atomic<TypedMessage*> mpsc_next{nullptr};

private:
    TypeTag tag_ = TypeTag::Invalid;
    StreamBuffer payload_;
    mutable std::shared_ptr<google::protobuf::Message> parsed_;
    ActorAddress sender_address_;
};

} // namespace hpactor
