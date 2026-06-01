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

#include <hpactor/adt/mpsc_ring_buffer.hpp>

namespace hpactor::metrics {

/// \brief Alias for backward compatibility — delegates to the shared ADT
///        MpscRingBuffer.
///
/// \tparam T Element type stored in the ring buffer.
/// \tparam Capacity Compile-time buffer capacity (must be a power of two).
template <typename T, size_t Capacity = 65536>
using MpscRingBuffer = adt::MpscRingBuffer<T, Capacity>;

} // namespace hpactor::metrics
