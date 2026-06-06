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

#include <cstdint>

namespace hpactor {

// Message identity
enum class TypeTag : uint32_t;
class TypedMessage;

// Delivery contracts
enum class FailureReason : uint8_t;
enum class FailureSource : uint8_t;
struct FailureEnvelope;
struct RequestTimeout;
template <typename T> class RequestHandle;

} // namespace hpactor

namespace hpactor::mailbox {

enum class DeliveryMode : uint8_t;
enum class DeliveryStatus : uint8_t;
struct DeliveryResult;
enum class EnqueueResultCode : uint8_t;
struct EnqueueResult;
class DedupCache;
enum class DeadLetterReason : uint8_t;
enum class DeadLetterSource : uint8_t;
struct DeadLetterRecord;

} // namespace hpactor::mailbox

namespace hpactor::net {

struct WireFrame;

} // namespace hpactor::net
