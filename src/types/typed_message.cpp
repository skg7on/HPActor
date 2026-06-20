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

#include <hpactor/msg/typed_message.hpp>

#include <cstring>

namespace hpactor {

TypedMessage::TypedMessage(TypeTag tag, const google::protobuf::Message& msg)
    : tag_(tag) {
    auto size = msg.ByteSizeLong();
    payload_.resize(size);
    (void)msg.SerializeToArray(payload_.data(), static_cast<int>(size));
}

TypedMessage TypedMessage::create_inline(TypeTag tag, const uint8_t* data,
                                         size_t data_len) noexcept {
    if (data_len <= kMaxInlinePayload) {
        TypedMessage msg;
        msg.tag_ = tag;
        std::memcpy(msg.inline_payload_, data, data_len);
        msg.inline_size_ = static_cast<uint8_t>(data_len);
        msg.is_inline_ = true;
        return msg;
    }
    // Fall back to heap-allocated StreamBuffer
    return TypedMessage(tag, StreamBuffer(data, data + data_len));
}

} // namespace hpactor
